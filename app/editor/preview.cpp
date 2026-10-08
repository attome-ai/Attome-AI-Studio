#include "preview.hpp"

#include <chrono>

#include "atm/base/profiler.hpp"

namespace atm::editor {

Preview::Preview() : thread_([this] { run(); }) {}

Preview::~Preview() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  wake_.notify_all();
  thread_.join();
}

void Preview::set_composition(render::Composition composition, int width, int height) {
  {
    std::lock_guard lock(mutex_);
    new_comp_ = std::make_unique<render::Composition>(std::move(composition));
    new_width_ = width;
    new_height_ = height;
    dirty_ = true;
  }
  wake_.notify_all();
}

void Preview::request(int64_t frame) {
  {
    std::lock_guard lock(mutex_);
    if (frame == wanted_)
      return;
    wanted_ = frame;
    dirty_ = true;
  }
  wake_.notify_all();
}

void Preview::live(std::function<void(render::Renderer &)> edit) {
  {
    std::lock_guard lock(mutex_);
    live_.push_back(std::move(edit));
    dirty_ = true;
  }
  wake_.notify_all();
}

void Preview::set_transform(const std::string &clip_id, const render::Transform &xf) {
  live([clip_id, xf](render::Renderer &r) { r.set_transform(clip_id, xf); });
}

void Preview::set_effect_param(const std::string &effect_id, int param, float value) {
  live([effect_id, param, value](render::Renderer &r) { r.set_effect_param(effect_id, param, value); });
}

void Preview::set_opacity(const std::string &clip_id, float opacity) {
  live([clip_id, opacity](render::Renderer &r) { r.set_opacity(clip_id, opacity); });
}

void Preview::show_composition(render::Composition composition) {
  auto shared = std::make_shared<render::Composition>(std::move(composition));
  live([shared](render::Renderer &r) { r.replace_composition(std::move(*shared)); });
}

void Preview::set_text(const std::string &clip_id, const std::string &text) {
  live([clip_id, text](render::Renderer &r) { r.set_text(clip_id, text); });
}

void Preview::set_text_style(const std::string &clip_id, float size, uint32_t color) {
  live([clip_id, size, color](render::Renderer &r) { r.set_text_style(clip_id, size, color); });
}

void Preview::set_gpu_device(int device) {
  {
    std::lock_guard lock(mutex_);
    if (device == gpu_wanted_)
      return;
    gpu_wanted_ = device;
    dirty_ = true;
  }
  wake_.notify_all();
}

std::string Preview::gpu_name() const {
  std::lock_guard lock(extent_mutex_);
  return gpu_used_;
}

std::pair<int, int> Preview::extent(const std::string &clip_id) const {
  std::lock_guard lock(extent_mutex_);
  const auto it = extents_.find(clip_id);
  return it == extents_.end() ? std::pair<int, int>{0, 0} : it->second;
}

bool Preview::take(std::vector<uint8_t> &bgrx, int &width, int &height, int64_t &frame, std::string &warning) {
  std::lock_guard lock(mutex_);
  if (!fresh_)
    return false;
  fresh_ = false;
  bgrx.swap(done_); // the worker allocates again only when the size changes
  width = done_width_;
  height = done_height_;
  frame = done_frame_;
  warning = done_warning_;
  return true;
}

void Preview::run() {
  prof::set_thread_name("ui-preview");
  std::unique_ptr<gpu::Context> gpu; // before the renderer, which may hold decoders on it: it goes after it
  std::unique_ptr<render::Renderer> renderer;
  int gpu_device = -1; // the device `gpu` was made for
  std::vector<uint8_t> buffer;
  for (;;) {
    int64_t frame = -1;
    int want_gpu = -1;
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, [&] { return stop_ || dirty_; });
      if (stop_)
        return;
      dirty_ = false;
      if (new_comp_) {
        renderer = std::make_unique<render::Renderer>(std::move(*new_comp_), new_width_, new_height_);
        new_comp_.reset();
        live_.clear(); // a rebuilt composition already holds the saved values
      }
      if (renderer)
        for (auto &edit : live_)
          edit(*renderer);
      live_.clear();
      frame = wanted_;
      want_gpu = gpu_wanted_;
    }
    if (want_gpu != gpu_device) { // a device made (or let go) here, outside the lock: it takes a moment
      if (renderer)
        renderer->use_gpu(nullptr);
      gpu.reset();
      gpu_device = want_gpu;
      if (want_gpu >= 0)
        if (auto made = gpu::Context::create(want_gpu))
          gpu = std::move(*made);
    }
    if (renderer)
      renderer->use_gpu(gpu.get());
    if (!renderer || frame < 0)
      continue;
    ATM_PROFILE_FRAME();
    const auto t0 = std::chrono::steady_clock::now();
    buffer.resize(media::nv12_size(renderer->width(), renderer->height()));
    const int64_t runs_before = renderer->gpu_runs();
    const auto rendered = renderer->render(frame, buffer.data());
    std::string warning = rendered ? renderer->take_warning() : rendered.error().message;
    {
      std::lock_guard lock(extent_mutex_);
      for (const render::Layer &l : renderer->composition().layers)
        if (l.is_text)
          extents_[l.clip_id] = renderer->text_extent(l.clip_id);
      gpu_used_ = renderer->gpu_runs() > runs_before && gpu ? gpu->info().device : std::string();
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::lock_guard lock(mutex_);
    done_.swap(buffer);
    done_width_ = renderer->width();
    done_height_ = renderer->height();
    done_frame_ = frame;
    if (!warning.empty())
      done_warning_ = std::move(warning);
    fresh_ = true;
    last_ms_ = ms;
  }
}

} // namespace atm::editor
