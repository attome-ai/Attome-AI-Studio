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

#include <vulkan/vulkan.h>

#include "atm/base/profiler.hpp"

namespace atm::gpu {
namespace {

const uint32_t kBoxSpirv[] = { // one pass along columns (or rows) with a running sum
#include "box.spv.inc"
};
const uint32_t kRowsSpirv[] = { // the three horizontal passes of a row at once, in shared memory
#include "box_rows.spv.inc"
};
constexpr uint32_t kMaxRowBytes = 8192; // box_rows.comp holds a row of up to this many bytes in shared memory

using Clock = std::chrono::steady_clock;
double us_since(Clock::time_point t0) { return std::chrono::duration<double, std::micro>(Clock::now() - t0).count(); }

tl::unexpected<Error> vk_fail(const char *what, VkResult r) {
  return fail(ErrorCode::EncoderUnavailable, "G_VULKAN", std::string("Vulkan: ") + what + " failed (" + std::to_string(int(r)) + ").", {},
              "The GPU path is not used; the CPU renderer does the work. Updating the graphics driver may help.");
}

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

} // namespace

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

struct Context::Impl {
  Info info;
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t family = 0;
  VkPhysicalDeviceMemoryProperties memory_props{};
  float timestamp_ns = 1.0f; // nanoseconds per timestamp tick
  bool timestamps = false;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  VkQueryPool queries = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkShaderModule shader = VK_NULL_HANDLE, rows_shader = VK_NULL_HANDLE;
  VkPipeline box = VK_NULL_HANDLE;
  std::map<uint64_t, VkPipeline> rows; // box_rows.comp by the row capacity and chunk it was made for
  uint32_t shared_limit = 0; // bytes of shared memory a workgroup can have
  VkDescriptorPool descriptors = VK_NULL_HANDLE;
  VkDescriptorSet a_to_b = VK_NULL_HANDLE, b_to_a = VK_NULL_HANDLE;
  Buffer a, b, upload, download;

  ~Impl() {
    if (device) {
      vkDeviceWaitIdle(device);
      for (Buffer *buf : {&a, &b, &upload, &download})
        release(*buf);
      vkDestroyDescriptorPool(device, descriptors, nullptr);
      vkDestroyPipeline(device, box, nullptr);
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

  void release(Buffer &buf) {
    if (buf.mapped)
      vkUnmapMemory(device, buf.memory);
    vkDestroyBuffer(device, buf.buffer, nullptr);
    vkFreeMemory(device, buf.memory, nullptr);
    buf = Buffer{};
  }

  // The first memory type that `bits` allows with all of `want`; with `nice` too when there is one.
  int memory_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags nice = 0) const {
    for (const VkMemoryPropertyFlags flags : {want | nice, want})
      for (uint32_t i = 0; i < memory_props.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memory_props.memoryTypes[i].propertyFlags & flags) == flags)
          return int(i);
    return -1;
  }

  // A buffer of at least `size` bytes; one that is already big enough is kept.
  Result<void> ensure(Buffer &buf, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags want, VkMemoryPropertyFlags nice = 0) {
    if (buf.buffer && buf.size >= size)
      return {};
    release(buf);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
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

  Result<void> ensure_buffers(VkDeviceSize size) {
    const bool grew = !a.buffer || a.size < size;
    constexpr VkBufferUsageFlags kWork = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    ATM_CHECK(ensure(a, size, kWork, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    ATM_CHECK(ensure(b, size, kWork, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    ATM_CHECK(ensure(upload, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
    // Read back by the CPU: cached memory is many times faster to read than write-combined.
    ATM_CHECK(ensure(download, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     VK_MEMORY_PROPERTY_HOST_CACHED_BIT));
    if (grew) { // the descriptor sets point at the new buffers
      const auto point = [&](VkDescriptorSet set, const Buffer &from, const Buffer &to) {
        VkDescriptorBufferInfo infos[2] = {{from.buffer, 0, VK_WHOLE_SIZE}, {to.buffer, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet writes[2]{};
        for (uint32_t i = 0; i < 2; ++i) {
          writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
          writes[i].dstSet = set;
          writes[i].dstBinding = i;
          writes[i].descriptorCount = 1;
          writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
          writes[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
      };
      point(a_to_b, a, b);
      point(b_to_a, b, a);
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
      const Device d = describe(devices[i], int(i));
      if (wanted >= 0 && int(i) == wanted && !d.usable)
        return fail(ErrorCode::EncoderUnavailable, "G_DEVICE_UNUSABLE", d.name + " cannot render: " + d.why_not + ".");
      if (!d.usable)
        continue;
      const int score = wanted >= 0 ? (int(i) == wanted ? 10 : 0) : d.discrete ? 3 : d.integrated ? 2 : 1;
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

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan13Features on13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    on13.synchronization2 = VK_TRUE; // the barriers and timestamps below are the synchronization2 kind
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.pNext = &on13;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    VK_TRY("vkCreateDevice", vkCreateDevice(physical, &di, nullptr, &device));
    vkGetDeviceQueue(device, family, 0, &queue);

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

    VkDescriptorSetLayoutBinding bindings[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
      bindings[i].binding = i;
      bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      bindings[i].descriptorCount = 1;
      bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 2;
    li.pBindings = bindings;
    VK_TRY("vkCreateDescriptorSetLayout", vkCreateDescriptorSetLayout(device, &li, nullptr, &set_layout));
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Pass)};
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


    VkDescriptorPoolSize sizes{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 2;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &sizes;
    VK_TRY("vkCreateDescriptorPool", vkCreateDescriptorPool(device, &dpi, nullptr, &descriptors));
    VkDescriptorSetLayout two[2] = {set_layout, set_layout};
    VkDescriptorSet sets[2];
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = descriptors;
    dai.descriptorSetCount = 2;
    dai.pSetLayouts = two;
    VK_TRY("vkAllocateDescriptorSets", vkAllocateDescriptorSets(device, &dai, sets));
    a_to_b = sets[0];
    b_to_a = sets[1];
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

  // The copies between the caller's packed picture (rows `W` bytes apart) and the device's (rows `P` apart, P >= W, a
  // multiple of 4): one copy when they are the same, one per row when the rows are padded.
  std::vector<VkBufferCopy> regions;
  const std::vector<VkBufferCopy> &copies(int W, int H, uint32_t P, bool to_device) {
    regions.clear();
    const VkDeviceSize luma = VkDeviceSize(W) * VkDeviceSize(H);
    if (P == uint32_t(W)) {
      regions.push_back({0, 0, luma + luma / 2});
      return regions;
    }
    for (int y = 0; y < H + H / 2; ++y) {
      const VkDeviceSize packed = VkDeviceSize(y) * VkDeviceSize(W), padded = VkDeviceSize(y) * P;
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

Result<void> Context::blur_nv12(uint8_t *nv12, int W, int H, float sigma, Timing *timing) {
  ATM_PROFILE_SCOPE("gpu.blur");
  Impl &m = *impl_;
  const auto t0 = Clock::now();
  const size_t luma = size_t(W) * size_t(H), size = luma + luma / 2;
  const int ry = box_radius(sigma), rc = box_radius(sigma * 0.5f);
  if (ry < 1 && rc < 1)
    return {};
  if (uint32_t(W) > kMaxRowBytes)
    return fail(ErrorCode::EncoderUnavailable, "G_TOO_WIDE", "The GPU blur takes pictures up to " + std::to_string(kMaxRowBytes) + " pixels wide.");
  const uint32_t P = (uint32_t(W) + 3u) & ~3u; // rows on the device: whole 4-byte words
  const uint32_t uh = uint32_t(H), chroma_at = P * uh;
  ATM_CHECK(m.ensure_buffers(VkDeviceSize(P) * (uh + uh / 2)));
  auto t = Clock::now();
  std::memcpy(m.upload.mapped, nv12, size);
  const double upload_us = us_since(t);

  uint32_t shift = 2; // bytes per invocation of the row passes: a power of two, at least a word
  while ((256u << shift) < uint32_t(W))
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
  const auto &in = m.copies(W, H, P, true);
  vkCmdCopyBuffer(m.cmd, m.upload.buffer, m.a.buffer, uint32_t(in.size()), in.data());
  m.barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
  if (m.timestamps)
    vkCmdWriteTimestamp2(m.cmd, VK_PIPELINE_STAGE_2_COPY_BIT, m.queries, 1);
  // The three horizontal passes at once, A -> B: a workgroup per row, the row in shared memory.
  const uint32_t uw = uint32_t(W);
  vkCmdBindDescriptorSets(m.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m.layout, 0, 1, &m.a_to_b, 0, nullptr);
  const auto rows_pass = [&](const RowsPass &p, uint32_t count) {
    vkCmdPushConstants(m.cmd, m.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
    vkCmdDispatch(m.cmd, count, 1, 1);
  };
  if (ry >= 1) {
    vkCmdBindPipeline(m.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, rows_luma);
    rows_pass({0, P, uw, 1, ry, shift, magic_for(ry)}, uh);
  }
  if (rc >= 1) {
    vkCmdBindPipeline(m.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, rows_chroma);
    rows_pass({chroma_at, P, uw, 2, rc, shift, magic_for(rc)}, uh / 2);
  }
  const auto compute_to_compute = [&] {
    m.barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
  };
  compute_to_compute();
  if (m.timestamps)
    vkCmdWriteTimestamp2(m.cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, m.queries, 4);
  // The three vertical passes, B -> A -> B -> A: an invocation per word column and stretch of rows, a running sum down it.
  // A stretch is the window's width (at least 16 rows): enough invocations to fill the GPU, and the window's first sum
  // costs no more than the stretch.
  const auto seg = [](int r) { return uint32_t(std::max(16, 2 * r + 1)); };
  vkCmdBindPipeline(m.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m.box);
  for (int pass = 0; pass < 3; ++pass) {
    vkCmdBindDescriptorSets(m.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m.layout, 0, 1, pass == 1 ? &m.a_to_b : &m.b_to_a, 0, nullptr);
    if (ry >= 1)
      m.dispatch({0, uh, P, P / 4, 1, ry, seg(ry), 1});
    if (rc >= 1)
      m.dispatch({chroma_at, uh / 2, P, P / 4, 1, rc, seg(rc), 1});
    compute_to_compute();
  }
  m.barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
  // A plane without a blur (radius 0) was not touched: it is still in A as it came, so the copy back is whole.
  if (m.timestamps)
    vkCmdWriteTimestamp2(m.cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, m.queries, 2);
  const auto &out = m.copies(W, H, P, false);
  vkCmdCopyBuffer(m.cmd, m.a.buffer, m.download.buffer, uint32_t(out.size()), out.data());
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
  VK_TRY("vkWaitForFences", vkWaitForFences(m.device, 1, &m.fence, VK_TRUE, UINT64_MAX));

  t = Clock::now();
  std::memcpy(nv12, m.download.mapped, size);
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
