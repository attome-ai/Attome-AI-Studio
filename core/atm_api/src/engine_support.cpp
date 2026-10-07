#include "engine_impl.hpp"

namespace atm::api {

// A generation as a job: units are steps (cached ones count at once), `detail` says which clip and step is running.
void run_gen(const std::shared_ptr<Job> &job, GenRun run, std::shared_ptr<FinishedQueue> queue, std::string project) {
  prof::set_thread_name("atm-generate");
  GenProgress progress;
  progress.done = &job->units_done;
  progress.cancel = &job->cancel;
  progress.on_detail = [&](const std::string &text) {
    std::lock_guard lock(job->mutex);
    job->detail = text;
  };
  progress.on_node = [&](const std::string &node, int at, int of) {
    std::lock_guard lock(job->mutex);
    job->node = node;
    job->node_at = at;
    job->node_of = of;
  };
  progress.on_clip = [&](const GenOutcome &out) {
    if (out.state != "done")
      return;
    std::lock_guard lock(queue->mutex);
    queue->items.push_back({project, out.clip, out.name, out.take});
  };
  const std::vector<GenOutcome> outcomes = run_generation(run, progress);
  json clips = json::array();
  const Error *first = nullptr;
  bool cancelled = false;
  for (const GenOutcome &out : outcomes) {
    json c = {{"clip", out.clip}, {"name", out.name}, {"state", out.state}, {"steps_run", out.ran}, {"steps_cached", out.cached}, {"seconds", out.seconds}};
    if (out.state == "skipped")
      c["why"] = out.why;
    if (out.state == "failed") {
      c["error"] = error_to_json(out.error);
      if (!out.node.empty())
        c["node"] = out.node; // the node of the clip's workflow the run stopped at
      if (!first)
        first = &out.error;
    }
    cancelled = cancelled || out.state == "cancelled";
    clips.push_back(std::move(c));
  }
  std::lock_guard lock(job->mutex);
  job->result = {{"clips", std::move(clips)}};
  job->detail.clear();
  job->seconds = std::chrono::duration<double>(Clock::now() - job->started).count();
  if (first)
    job->error = *first;
  job->state.store(cancelled ? Job::cancelled : first ? Job::failed : Job::done);
}

// A model download as a job: units are bytes, `detail` says which file is being downloaded or checked. Stopping it
// keeps what arrived; the next models.fetch of the same entry continues from there.
void run_fetch(const std::shared_ptr<Job> &job, models::CatalogEntry entry, fs::path dir, std::vector<fs::path> also,
               std::shared_ptr<net::Transport> transport) {
  prof::set_thread_name("atm-fetch");
  models::FetchProgress progress;
  progress.done = &job->units_done;
  progress.fetched = &job->fetched;
  progress.cancel = &job->cancel;
  progress.on_phase = [&](const std::string &text) {
    std::lock_guard lock(job->mutex);
    job->detail = text;
  };
  const auto result = models::fetch_entry(*transport, entry, dir, progress, {}, also);
  std::lock_guard lock(job->mutex);
  job->seconds = std::chrono::duration<double>(Clock::now() - job->started).count();
  if (result) {
    job->detail = "Installed";
    job->state.store(Job::done);
  } else if (result.error().code == ErrorCode::Cancelled) {
    job->detail = "Stopped; start it again to continue";
    job->state.store(Job::cancelled);
  } else {
    job->error = result.error();
    job->state.store(Job::failed);
  }
}

// A little-endian 16-bit stereo WAV of `stereo` (interleaved floats), from stereo frame `from` to `to`.
std::string wav_bytes(const std::vector<float> &stereo, size_t from, size_t to) {
  const size_t frames = to > from ? to - from : 0;
  std::string wav(44 + frames * 4, '\0');
  const auto put = [&](size_t at, uint32_t v, int bytes) {
    for (int i = 0; i < bytes; ++i)
      wav[at + size_t(i)] = char((v >> (8 * i)) & 255);
  };
  wav.replace(0, 4, "RIFF");
  put(4, uint32_t(36 + frames * 4), 4);
  wav.replace(8, 8, "WAVEfmt ");
  put(16, 16, 4);
  put(20, 1, 2); // PCM
  put(22, 2, 2); // stereo
  put(24, uint32_t(media::kAudioRate), 4);
  put(28, uint32_t(media::kAudioRate * 4), 4);
  put(32, 4, 2);
  put(34, 16, 2);
  wav.replace(36, 4, "data");
  put(40, uint32_t(frames * 4), 4);
  for (size_t i = 0; i < frames * 2; ++i) {
    const float v = std::clamp(stereo[from * 2 + i], -1.0f, 1.0f);
    put(44 + i * 2, uint32_t(uint16_t(int16_t(std::lround(v * 32767.0f)))), 2);
  }
  return wav;
}

Result<SequenceRef> sequence_of(const json &root, const json &params) {
  std::string id = params.value("sequence", std::string());
  if (id.empty() && root.contains("sequence_order") && !root["sequence_order"].empty())
    id = root["sequence_order"][0].get<std::string>();
  if (!root.contains("sequences") || !root["sequences"].contains(id))
    return fail(ErrorCode::NotFound, "R_NO_SEQUENCE", "The project has no sequence \"" + id + "\".");
  return SequenceRef{id, &root["sequences"][id]};
}

// The sequence a clip or a track is in (empty when no sequence holds it): a Tool that is given a clip works in that clip's sequence, so it needs no "sequence" beside it.
std::string sequence_holding(const json &root, const std::string &clip_id) {
  const auto seqs = root.find("sequences");
  if (seqs == root.end() || !seqs->is_object())
    return {};
  for (auto sq = seqs->begin(); sq != seqs->end(); ++sq) {
    const auto tracks = sq->find("tracks");
    if (!sq->is_object() || tracks == sq->end() || !tracks->is_object())
      continue;
    if (tracks->contains(clip_id))
      return sq.key();
    for (auto t = tracks->begin(); t != tracks->end(); ++t)
      if (t->is_object() && t->contains("clips") && (*t)["clips"].contains(clip_id))
        return sq.key();
  }
  return {};
}

// "Sound only" and "a picture of one frame" as jobs: nothing to encode, so one unit.
void run_extract(const std::shared_ptr<Job> &job, render::Composition comp, std::string path, bool sound, int64_t first, int64_t last, int width, int height) {
  prof::set_thread_name("atm-render-0");
  const auto finish = [&](Job::State state, const Error *error = nullptr) {
    std::lock_guard lock(job->mutex);
    if (error)
      job->error = *error;
    job->seconds = std::chrono::duration<double>(Clock::now() - job->started).count();
    job->state.store(state);
  };
  if (sound) {
    auto mixed = render::mix_audio(comp);
    if (!mixed)
      return finish(Job::failed, &mixed.error());
    const size_t total = mixed->size() / 2;
    const auto at = [&](int64_t frame) { return std::min(total, size_t(comp.frame_hns(frame) * media::kAudioRate / media::kHnsPerSecond)); };
    if (auto written = storage::atomic_write(to_path(path), wav_bytes(*mixed, at(first), at(last))); !written)
      return finish(Job::failed, &written.error());
  } else {
    render::Renderer renderer(std::move(comp), width, height);
    std::vector<uint8_t> nv12(media::nv12_size(renderer.width(), renderer.height()));
    if (auto r = renderer.render(first, nv12.data()); !r)
      return finish(Job::failed, &r.error());
    std::vector<uint8_t> bgrx(size_t(renderer.width()) * size_t(renderer.height()) * 4);
    media::nv12_to_bgrx(nv12.data(), renderer.width(), renderer.height(), bgrx.data());
    if (auto r = media::write_jpeg(path, bgrx.data(), renderer.width() & ~1, renderer.height() & ~1, 0.95f); !r)
      return finish(Job::failed, &r.error());
  }
  job->units_done.store(1);
  finish(Job::done);
}

// An image sequence: each frame of the range as a numbered PNG in `dir` (name_000000.png, numbered by the frame of the sequence). One thread renders
// and writes, a frame at a time; the job can be cancelled between frames.
void run_png_sequence(const std::shared_ptr<Job> &job, render::Composition comp, std::string dir, std::string name, int64_t first, int64_t last, int width, int height) {
  prof::set_thread_name("atm-render-0");
  const auto finish = [&](Job::State state, const Error *error = nullptr) {
    std::lock_guard lock(job->mutex);
    if (error)
      job->error = *error;
    job->seconds = std::chrono::duration<double>(Clock::now() - job->started).count();
    job->state.store(state);
  };
  render::Renderer renderer(std::move(comp), width, height);
  const int w = renderer.width() & ~1, h = renderer.height() & ~1;
  std::vector<uint8_t> nv12(media::nv12_size(renderer.width(), renderer.height())), bgrx(size_t(renderer.width()) * size_t(renderer.height()) * 4);
  for (int64_t frame = first; frame < last; ++frame) {
    if (job->cancel.load())
      return finish(Job::cancelled);
    ATM_PROFILE_SCOPE("render.png_frame");
    if (auto r = renderer.render(frame, nv12.data()); !r)
      return finish(Job::failed, &r.error());
    media::nv12_to_bgrx(nv12.data(), renderer.width(), renderer.height(), bgrx.data());
    char number[16];
    std::snprintf(number, sizeof number, "%06lld", static_cast<long long>(frame));
    if (auto r = media::write_png(dir + "/" + name + "_" + number + ".png", bgrx.data(), w, h); !r)
      return finish(Job::failed, &r.error());
    job->units_done.fetch_add(1);
  }
  finish(Job::done);
}

// The export runs as a pipeline: this thread renders frames into a few slots while "atm-encode" converts and
// encodes the previous ones, so the two never wait for each other. The encoder starts (hardware set-up, about half a
// second) and the audio is mixed while the first frames render.
namespace {
// Where the finished frames and sound go: the operating system's H.264 encoder, or the user's FFmpeg for ProRes and DNxHR.
struct Sink {
  std::unique_ptr<media::Encoder> os;
  std::unique_ptr<media::PipeEncoder> pipe;
  Sink *operator->() { return this; }
  Result<void> video(const uint8_t *nv12, int64_t frame) { return pipe ? pipe->video(nv12, frame) : os->video(nv12, frame); }
  Result<void> audio(const float *stereo, size_t frames) { return pipe ? pipe->audio(stereo, frames) : os->audio(stereo, frames); }
  Result<void> finish() { return pipe ? pipe->finish() : os->finish(); }
  const std::string &name() const { return pipe ? pipe->name() : os->name(); }
};
Result<Sink> make_sink(const media::EncodeSettings &settings) {
  Sink sink;
  if (settings.codec != "h264") {
    ATM_TRY(auto pipe, media::PipeEncoder::create(settings));
    sink.pipe = std::move(pipe);
  } else {
    ATM_TRY(auto os, media::Encoder::create(settings));
    sink.os = std::move(os);
  }
  return sink;
}
} // namespace

void run_export(const std::shared_ptr<Job> &job, render::Composition comp, media::EncodeSettings settings, int64_t first, int64_t last) {
  prof::set_thread_name("atm-render-0");
  const auto finish = [&](Job::State state, const Error *error = nullptr) {
    std::lock_guard lock(job->mutex);
    if (error)
      job->error = *error;
    job->seconds = std::chrono::duration<double>(Clock::now() - job->started).count();
    job->state.store(state);
  };
  render::Renderer renderer(comp, settings.width, settings.height);
  settings.width = renderer.width();
  settings.height = renderer.height();

  constexpr int kSlots = 4;
  struct Slot {
    std::vector<uint8_t> pixels;
    int64_t frame = -1;
  };
  std::vector<Slot> slots(kSlots);
  for (Slot &slot : slots)
    slot.pixels.resize(media::nv12_size(settings.width, settings.height));
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<int> ready, free_slots = {0, 1, 2, 3};
  bool render_done = false, failed = false;
  Error error;

  std::thread encode_thread([&] {
    prof::set_thread_name("atm-encode");
    const auto stop = [&](Error e) {
      std::lock_guard lock(mutex);
      if (!failed)
        error = std::move(e);
      failed = true;
      cv.notify_all();
    };
    std::vector<float> audio;
    std::thread mixer;
    if (settings.audio)
      mixer = std::thread([&] {
        prof::set_thread_name("atm-audio-mix");
        if (auto mixed = render::mix_audio(comp))
          audio = std::move(*mixed);
      });
    auto encoder = make_sink(settings);
    if (mixer.joinable())
      mixer.join();
    if (!encoder)
      return stop(encoder.error());
    {
      std::lock_guard lock(job->mutex);
      job->encoder = (*encoder)->name();
    }
    const size_t audio_frames = audio.size() / 2;
    size_t audio_pos = std::min(audio_frames, size_t(comp.frame_hns(first) * media::kAudioRate / media::kHnsPerSecond)); // stereo frames already written
    for (;;) {
      int index = -1;
      {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return failed || !ready.empty() || render_done; });
        if (failed)
          return;
        if (ready.empty())
          break; // rendering finished and everything is encoded
        index = ready.front();
        ready.pop_front();
      }
      const int64_t f = slots[size_t(index)].frame;
      Result<void> r = (*encoder)->video(slots[size_t(index)].pixels.data(), f - first);
      {
        std::lock_guard lock(mutex);
        free_slots.push_back(index);
      }
      cv.notify_all();
      // Audio is interleaved about half a second at a time, so the muxer never holds much of one stream.
      const size_t audio_to =
          std::min(audio_frames, size_t(comp.frame_hns(f + 1) * media::kAudioRate / media::kHnsPerSecond));
      if (r && (audio_to - audio_pos >= size_t(media::kAudioRate / 2) || f + 1 == last) && audio_to > audio_pos) {
        r = (*encoder)->audio(audio.data() + audio_pos * 2, audio_to - audio_pos);
        audio_pos = audio_to;
      }
      if (!r)
        return stop(r.error());
      job->units_done.store(f + 1 - first);
    }
    if (auto r = (*encoder)->finish(); !r)
      stop(r.error());
  });

  bool cancelled = false;
  for (int64_t f = first; f < last; ++f) {
    int index = -1;
    {
      std::unique_lock lock(mutex);
      cv.wait(lock, [&] { return failed || !free_slots.empty(); });
      if (failed)
        break;
      index = free_slots.front();
      free_slots.pop_front();
    }
    if (job->cancel.load()) {
      cancelled = true;
      std::lock_guard lock(mutex);
      failed = true; // stops the encoder thread
      cv.notify_all();
      break;
    }
    ATM_PROFILE_FRAME(); // one video frame = one frame of the render thread
    Result<void> r = renderer.render(f, slots[size_t(index)].pixels.data());
    std::lock_guard lock(mutex);
    if (!r) {
      if (!failed)
        error = r.error();
      failed = true;
      cv.notify_all();
      break;
    }
    slots[size_t(index)].frame = f;
    ready.push_back(index);
    cv.notify_all();
  }
  {
    std::lock_guard lock(mutex);
    render_done = true;
  }
  cv.notify_all();
  encode_thread.join();

  if (cancelled) {
    std::error_code ec;
    fs::remove(to_path(settings.path), ec);
    return finish(Job::cancelled);
  }
  if (failed)
    return finish(Job::failed, &error);
  {
    std::lock_guard lock(job->mutex);
    job->warning = renderer.take_warning();
  }
  finish(Job::done);
}

} // namespace atm::api
