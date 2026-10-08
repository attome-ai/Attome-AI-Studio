// Where rendering runs (docs/plan/GPU_RENDERER.md §3): Automatic, a named GPU, or the CPU only. render.devices and render.set_device.
#include "engine_impl.hpp"

#include "atm/gpu/gpu.hpp"

namespace atm::api {

namespace {

// A GPU as the settings remember it: by name and the vendor's ids, not by Vulkan's order (that can change).
json device_key(const gpu::Device &d) { return {{"name", d.name}, {"vendor_id", d.vendor_id}, {"device_id", d.device_id}}; }
bool same_device(const json &key, const gpu::Device &d) {
  return key.is_object() && key.value("name", std::string()) == d.name && key.value("vendor_id", 0u) == d.vendor_id && key.value("device_id", 0u) == d.device_id;
}
std::string check_key(const gpu::Device &d) { return d.name + " | " + d.driver; }

} // namespace

const std::vector<gpu::Device> &Engine::Impl::gpus(bool refresh) {
  if (!gpu_list || refresh)
    gpu_list = gpu::list_devices();
  return *gpu_list;
}

json Engine::Impl::render_device_now() { return device_in_use(gpus(), render_choice()); }

int Engine::Impl::gpu_device_for(const render::Composition &comp) {
  const bool effects = std::any_of(comp.layers.begin(), comp.layers.end(), [](const render::Layer &l) { return !l.effects.empty(); });
  return effects ? render_device_now().value("index", -1) : -1;
}

gpu::Context *Engine::Impl::gpu_for_stills() {
  const int device = render_device_now().value("index", -1);
  if (device != still_gpu_device) {
    still_gpu.reset();
    still_gpu_device = device;
    if (device >= 0)
      if (auto made = gpu::Context::create(device))
        still_gpu = std::move(*made);
  }
  return still_gpu.get();
}

json Engine::Impl::render_choice() const {
  const json all = settings();
  if (all.contains("render_device"))
    return all["render_device"];
  return chosen_render_device.is_null() ? json("auto") : chosen_render_device;
}

// The timing check of the plan: one 1080p blur (the effect that is slowest on the CPU) on the GPU and on the CPU, the best
// of three each, after one call that makes the GPU's buffers. Remembered per device and driver.
json Engine::Impl::device_check(const gpu::Device &d, bool fresh) {
  const std::string key = check_key(d);
  if (!fresh) {
    const json saved = settings().value("render_device_checks", json::object());
    if (saved.contains(key))
      return saved[key];
    if (device_checks.contains(key))
      return device_checks[key];
  }
  ATM_PROFILE_SCOPE("api.device_check");
  json out = json::object();
  const int W = 1920, H = 1080;
  const float sigma = 10.8f;
  std::vector<uint8_t> frame(media::nv12_size(W, H));
  for (size_t i = 0; i < frame.size(); ++i)
    frame[i] = uint8_t(16 + (i * 7919u) % 220u);
  auto ctx = gpu::Context::create(d.index);
  if (!ctx) {
    out = {{"ok", false}, {"error", ctx.error().message}};
  } else {
    std::vector<uint8_t> work = frame;
    (void)(*ctx)->blur_nv12(work.data(), W, H, sigma);
    double gpu_best = 1e30, cpu_best = 1e30;
    for (int i = 0; i < 3; ++i) {
      work = frame;
      const auto t0 = Clock::now();
      if (!(*ctx)->blur_nv12(work.data(), W, H, sigma))
        break;
      gpu_best = std::min(gpu_best, std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
      work = frame;
      const auto t1 = Clock::now();
      render::blur_picture(work.data(), W, H, sigma);
      cpu_best = std::min(cpu_best, std::chrono::duration<double, std::milli>(Clock::now() - t1).count());
    }
    out = {{"ok", gpu_best < 1e29}, {"gpu_ms", std::round(gpu_best * 100.0) / 100.0}, {"cpu_ms", std::round(cpu_best * 100.0) / 100.0}};
  }
  device_checks[key] = out;
  if (const fs::path path = settings_path(); !path.empty()) {
    json all = settings();
    all["render_device_checks"][key] = out;
    if (storage::make_dirs(path.parent_path()))
      (void)storage::atomic_write(path, all.dump(2) + "\n");
  }
  return out;
}

// What renders now and why: the choice applied to the devices that are here and to what is running.
json Engine::Impl::device_in_use(const std::vector<gpu::Device> &devices, const json &choice) {
  const auto cpu = [](std::string why) { return json{{"device", "CPU"}, {"index", -1}, {"reason", std::move(why)}}; };
  const auto gpu_of = [](const gpu::Device &d, std::string why) { return json{{"device", d.name}, {"index", d.index}, {"reason", std::move(why)}}; };
  if (choice.is_string() && choice.get<std::string>() == "cpu")
    return cpu("you chose the CPU");
  if (choice.is_object()) { // a named GPU: used when it is here, never overridden
    for (const gpu::Device &d : devices)
      if (same_device(choice, d))
        return d.usable ? gpu_of(d, "you chose it") : cpu(d.name + " cannot render: " + d.why_not);
    return cpu(choice.value("name", std::string("The chosen GPU")) + " is not on this computer now");
  }
  // Automatic.
  const bool generating = std::any_of(jobs.begin(), jobs.end(), [](const auto &j) { return j.second->kind == "gen.run" && j.second->state.load() == Job::running; });
  if (generating)
    return cpu("a generation is running: the GPU is left to it");
  const gpu::Device *discrete = nullptr;
  for (const gpu::Device &d : devices)
    if (d.usable && d.discrete && (!discrete || d.memory_mb > discrete->memory_mb))
      discrete = &d;
  if (discrete)
    return gpu_of(*discrete, "the discrete GPU");
  for (const gpu::Device &d : devices) // an integrated GPU only when it beats the CPU
    if (d.usable && d.integrated) {
      const json check = device_check(d, false);
      if (check.value("ok", false) && check.value("gpu_ms", 1e9) < check.value("cpu_ms", 0.0))
        return gpu_of(d, "faster than the CPU in the timing check");
      char why[160];
      std::snprintf(why, sizeof why, "the CPU is faster than %s (%.1f ms against %.1f ms in the timing check)", d.name.c_str(), check.value("cpu_ms", 0.0), check.value("gpu_ms", 0.0));
      return cpu(why);
    }
  return cpu("there is no GPU with Vulkan 1.3");
}

Result<json> Engine::Impl::render_devices(const json &params) {
  ATM_PROFILE_SCOPE("api.render_devices");
  const std::vector<gpu::Device> &devices = gpus(params.value("refresh", true)); // refresh false: the list made before (asked often)
  const bool fresh = params.value("check", false);
  json list = json::array();
  for (const gpu::Device &d : devices) {
    json one = {{"index", d.index}, {"name", d.name}, {"driver", d.driver}, {"discrete", d.discrete}, {"integrated", d.integrated},
                {"memory_mb", d.memory_mb}, {"usable", d.usable}};
    if (!d.usable)
      one["why_not"] = d.why_not;
    if (d.usable && (fresh || d.integrated)) // a discrete GPU needs no check to be picked; it is measured when asked
      one["check"] = device_check(d, fresh);
    else if (const json saved = settings().value("render_device_checks", json::object()); saved.contains(check_key(d)))
      one["check"] = saved[check_key(d)];
    list.push_back(std::move(one));
  }
  const json choice = render_choice();
  json out = {{"choice", choice.is_object() ? json(choice.value("name", std::string())) : choice}, {"devices", std::move(list)}, {"in_use", device_in_use(devices, choice)}};
  // Honest about today: only part of the picture is made on the GPU so far.
  out["note"] = "On the GPU today: the effects of a clip that fills the frame, and of an adjustment layer, when each of them has a GPU version (blur, "
                "sharpen, colour grade, vignette, film grain). Everything else is made on the CPU for now.";
  return out;
}

Result<json> Engine::Impl::render_set_device(const json &params) {
  ATM_PROFILE_SCOPE("api.render_set_device");
  if (!params.contains("device"))
    return bad_param("device", "is required: \"auto\", \"cpu\", or a GPU's name or index from render.devices");
  const json &want = params["device"];
  json choice;
  if (want.is_string() && (want == "auto" || want == "cpu")) {
    choice = want;
  } else {
    const std::vector<gpu::Device> &devices = gpus(true);
    const gpu::Device *found = nullptr;
    for (const gpu::Device &d : devices)
      if ((want.is_number_integer() && want.get<int>() == d.index) || (want.is_string() && want.get<std::string>() == d.name))
        found = &d;
    if (!found)
      return fail(ErrorCode::NotFound, "E_NO_DEVICE", "There is no GPU " + want.dump() + " on this computer.", {}, "render.devices lists them; or pass \"auto\" or \"cpu\".");
    if (!found->usable)
      return fail(ErrorCode::InvalidArgument, "E_DEVICE_UNUSABLE", found->name + " cannot render: " + found->why_not + ".");
    choice = device_key(*found);
  }
  chosen_render_device = choice;
  if (const fs::path path = settings_path(); !path.empty()) {
    json all = settings();
    all["render_device"] = choice;
    ATM_CHECK(storage::make_dirs(path.parent_path()));
    ATM_CHECK(storage::atomic_write(path, all.dump(2) + "\n"));
  }
  return render_devices(json::object());
}

} // namespace atm::api
