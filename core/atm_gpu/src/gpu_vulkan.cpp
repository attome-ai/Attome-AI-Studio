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
#include <vector>

#include <vulkan/vulkan.h>

#include "atm/base/profiler.hpp"

namespace atm::gpu {
namespace {

const uint32_t kBoxSpirv[] = {
#include "box.spv.inc"
};

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

// The push constants of box.comp, in its order.
struct Pass {
  uint32_t offset, rows, stride, n, channels;
  int32_t r;
  uint32_t seg, vertical;
};

} // namespace

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
  VkShaderModule shader = VK_NULL_HANDLE;
  VkPipeline box = VK_NULL_HANDLE;
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
      vkDestroyShaderModule(device, shader, nullptr);
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

  Result<void> init() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Attome";
    app.pEngineName = "Attome";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VK_TRY("vkCreateInstance", vkCreateInstance(&ici, nullptr, &instance));

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());
    // A device that can: Vulkan 1.3 with synchronization2, a compute queue. The discrete one first, unless one is named.
    int named = -1;
    if (const char *pick = std::getenv("ATTOME_GPU_DEVICE"); pick && *pick)
      named = std::atoi(pick);
    int best = -1, best_score = -1;
    for (uint32_t i = 0; i < count; ++i) {
      VkPhysicalDeviceProperties props;
      vkGetPhysicalDeviceProperties(devices[i], &props);
      if (props.apiVersion < VK_API_VERSION_1_3)
        continue;
      VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
      VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
      f2.pNext = &f13;
      vkGetPhysicalDeviceFeatures2(devices[i], &f2);
      if (!f13.synchronization2)
        continue;
      const int score = named >= 0 ? (int(i) == named ? 10 : 0) : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
      if (score > best_score) {
        best = int(i);
        best_score = score;
      }
    }
    if (best < 0)
      return fail(ErrorCode::EncoderUnavailable, "G_NO_DEVICE", "No GPU with Vulkan 1.3 was found.", {},
                  "The CPU renderer does the work. A recent graphics driver adds Vulkan 1.3 on most GPUs from 2016 on.");
    physical = devices[size_t(best)];
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
      qpi.queryCount = 4;
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

Result<std::unique_ptr<Context>> Context::create() {
  ATM_PROFILE_SCOPE("gpu.create");
  if (const char *off = std::getenv("ATTOME_GPU"); off && std::string(off) == "off")
    return fail(ErrorCode::EncoderUnavailable, "G_OFF", "The GPU path is switched off (ATTOME_GPU=off).");
  auto self = std::unique_ptr<Context>(new Context());
  self->impl_ = std::make_unique<Impl>();
  ATM_CHECK(self->impl_->init());
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
  const uint32_t P = (uint32_t(W) + 3u) & ~3u; // rows on the device: whole 4-byte words
  const uint32_t uh = uint32_t(H), chroma_at = P * uh;
  ATM_CHECK(m.ensure_buffers(VkDeviceSize(P) * (uh + uh / 2)));
  auto t = Clock::now();
  std::memcpy(m.upload.mapped, nv12, size);
  const double upload_us = us_since(t);

  VK_TRY("vkResetCommandBuffer", vkResetCommandBuffer(m.cmd, 0));
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VK_TRY("vkBeginCommandBuffer", vkBeginCommandBuffer(m.cmd, &bi));
  if (m.timestamps) {
    vkCmdResetQueryPool(m.cmd, m.queries, 0, 4);
    vkCmdWriteTimestamp2(m.cmd, VK_PIPELINE_STAGE_2_NONE, m.queries, 0);
  }
  const auto &in = m.copies(W, H, P, true);
  vkCmdCopyBuffer(m.cmd, m.upload.buffer, m.a.buffer, uint32_t(in.size()), in.data());
  m.barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
  if (m.timestamps)
    vkCmdWriteTimestamp2(m.cmd, VK_PIPELINE_STAGE_2_COPY_BIT, m.queries, 1);
  vkCmdBindPipeline(m.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m.box);
  // Each invocation slides along at least 64 samples, and at least the window's width (the window's first sum then costs
  // no more than the stretch itself); a multiple of 4, so a horizontal stretch ends on a word.
  const auto seg = [](int r) { return (uint32_t(std::max(64, 2 * r + 1)) + 3u) & ~3u; };
  const uint32_t uw = uint32_t(W);
  for (int pass = 0; pass < 3; ++pass) {
    // Horizontal, A -> B: the luma rows; the chroma rows with U and V together.
    vkCmdBindDescriptorSets(m.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m.layout, 0, 1, &m.a_to_b, 0, nullptr);
    if (ry >= 1)
      m.dispatch({0, uh, P, uw, 1, ry, seg(ry), 0});
    if (rc >= 1)
      m.dispatch({chroma_at, uh / 2, P, uw / 2, 2, rc, seg(rc), 0});
    m.barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
              VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    // Vertical, B -> A: 4 byte columns per invocation.
    vkCmdBindDescriptorSets(m.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m.layout, 0, 1, &m.b_to_a, 0, nullptr);
    if (ry >= 1)
      m.dispatch({0, uh, P, P / 4, 1, ry, seg(ry), 1});
    if (rc >= 1)
      m.dispatch({chroma_at, uh / 2, P, P / 4, 1, rc, seg(rc), 1});
    m.barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
              VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
  }
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
      uint64_t ticks[4] = {};
      vkGetQueryPoolResults(m.device, m.queries, 0, 4, sizeof ticks, ticks, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
      const auto us = [&](uint64_t from, uint64_t to) { return double(to - from) * double(m.timestamp_ns) / 1000.0; };
      timing->compute_us = us(ticks[1], ticks[2]);
      timing->gpu_copy_us = us(ticks[0], ticks[1]) + us(ticks[2], ticks[3]);
    }
    timing->total_us = us_since(t0);
  }
  return {};
}

} // namespace atm::gpu

#endif
