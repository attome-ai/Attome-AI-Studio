#pragma once
// The engine's internals, shared by the files that define its Tools. engine.hpp is the public face. This is what the Tools are made of: the Project, Job
// and queue types, a few helpers, and `Engine::Impl` with every Tool handler declared. A handler is defined in the file of its family (engine_*.cpp);
// the table that names them is in engine_tools.cpp.

#include "atm/api/engine.hpp"
#include "atm/api/audio_tools.hpp"
#include "atm/asr/asr.hpp"
#include "atm/api/script_plan.hpp"
#include "atm/api/edl.hpp"
#include "atm/api/subtitles.hpp"
#include <functional>
#include "atm/api/gen_comfy.hpp"
#include "atm/api/gen_mock.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <thread>
#include <unordered_map>

#include "atm/base/hash.hpp"
#include "atm/base/id.hpp"
#include "atm/base/profiler.hpp"
#include "atm/base/time.hpp"
#include "atm/doc/document.hpp"
#include "atm/gpu/gpu.hpp"
#include "atm/gen/library.hpp"
#include "atm/skills/skills.hpp"
#include "atm/gen/models.hpp"
#include "atm/gen/plan.hpp"
#include "atm/models/models.hpp"
#include "atm/net/http.hpp"
#include "atm/patch/history.hpp"
#include "atm/patch/patch.hpp"
#include "atm/render/render.hpp"
#include "atm/storage/file.hpp"
#include "generate.hpp"
#include "timeline.hpp"

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace atm::api {

namespace fs = std::filesystem;

using Clock = std::chrono::steady_clock;

using patch::ChangeSet;

inline fs::path to_path(const std::string &utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }

inline std::string to_utf8(const fs::path &p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}

inline int process_id() {
#if defined(_WIN32)
  return _getpid();
#else
  return int(getpid());
#endif
}

inline tl::unexpected<Error> bad_param(const char *key, const char *what) {
  return fail(ErrorCode::InvalidArgument, "E_PARAM", std::string("The parameter \"") + key + "\" " + what + ".", {},
              "See the parameters with: attome tools");
}

inline Result<const std::string *> string_param(const json &params, const char *key) {
  const auto it = params.find(key);
  if (it == params.end() || !it->is_string())
    return bad_param(key, "is required and must be a string");
  return &it->get_ref<const std::string &>();
}

struct Project {
  fs::path dir;
  std::string key;
  doc::Document doc;
  patch::History hist;
  storage::RecordLog journal;
  storage::File lock;
  uint64_t revision = 0;
  bool dirty = false;
  std::string save_error; // why the last automatic save failed ("" when it did not): the editor says so beside "Saved"
  Clock::time_point last_change{};
  size_t recovered = 0;   // journal records replayed on open (edits that were not yet in project.json)
  bool new_epoch = false; // project.json was changed outside Attome; the old history was dropped
};

// A long task that runs beside the writer thread on its own snapshot of the data (MODULES §A.3.2: long work
// returns a job ID at once; the result is fetched with jobs.get).
struct Job {
  enum State { running, done, failed, cancelled };
  std::string id, kind, output, encoder;
  std::string detail; // what the job is doing now, in words (a download: which file); guarded by `mutex`
  std::string node;   // a generation: the node of the clip's workflow that is running, and how far it is; guarded by `mutex`
  int node_at = 0, node_of = 0;
  std::atomic<int> state{running};
  std::atomic<int64_t> units_done{0}, units_total{0};
  std::atomic<int64_t> fetched{0}; // a download: bytes that arrived in this run (units_done also counts what was on disk)
  std::atomic<bool> cancel{false};
  std::mutex mutex; // guards error, warning, seconds
  Error error;
  std::string warning;
  std::string rendered_on; // a render: the device its effects ran on ("CPU" or a GPU's name); guarded by `mutex`
  double seconds = 0.0;
  json result; // what the job made, when it is more than one file (a generation: how each clip ended); guarded by `mutex`
  Clock::time_point started = Clock::now();
  std::thread thread;
};

// Takes that job threads finished. The document has one writer, so a Take is put on its clip by the engine's own
// thread, at its next call.
struct Finished {
  std::string project, clip, name;
  json take;
};

struct FinishedQueue {
  std::mutex mutex;
  std::vector<Finished> items;
};



// A generation as a job: units are steps (cached ones count at once), `detail` says which clip and step is running.
void run_gen(const std::shared_ptr<Job> &job, GenRun run, std::shared_ptr<FinishedQueue> queue, std::string project);



// A model download as a job: units are bytes, `detail` says which file is being downloaded or checked. Stopping it
// keeps what arrived; the next models.fetch of the same entry continues from there.
void run_fetch(const std::shared_ptr<Job> &job, models::CatalogEntry entry, fs::path dir, std::vector<fs::path> also,
               std::shared_ptr<net::Transport> transport);

// A transcription as a job (asr.transcribe): the sound of [from_s, from_s + duration_s) of `path` goes to the speech program; the words, with
// their times divided by `speed` (a clip's times are film time), are the job's result. `clip` is the clip they came from, or empty.
void run_asr(const std::shared_ptr<Job> &job, asr::Options options, std::string path, double from_s, double duration_s, double speed,
             std::string clip);



// A little-endian 16-bit stereo WAV of `stereo` (interleaved floats), from stereo frame `from` to `to`.
std::string wav_bytes(const std::vector<float> &stereo, size_t from, size_t to);

// The sequence a call works on: the one named in params ("sequence"), else the project's first. Every Tool that edits or reads a sequence asks here.
struct SequenceRef {
  std::string id;
  const json *node = nullptr;
};

Result<SequenceRef> sequence_of(const json &root, const json &params);



// The sequence a clip or a track is in (empty when no sequence holds it): a Tool that is given a clip works in that clip's sequence, so it needs no "sequence" beside it.
std::string sequence_holding(const json &root, const std::string &clip_id);

// Every clip of every track of every sequence: fn(clip ID, clip, track ID, track). One place for the walk, in the order of the document.
template <class F> void for_each_clip(const json &root, F &&fn) {
  const auto seqs = root.find("sequences");
  if (seqs == root.end() || !seqs->is_object())
    return;
  for (auto sq = seqs->begin(); sq != seqs->end(); ++sq) {
    const auto tracks = sq->find("tracks");
    if (!sq->is_object() || tracks == sq->end() || !tracks->is_object())
      continue;
    for (auto t = tracks->begin(); t != tracks->end(); ++t) {
      const auto clips = t->find("clips");
      if (!t->is_object() || clips == t->end() || !clips->is_object())
        continue;
      for (auto c = clips->begin(); c != clips->end(); ++c)
        if (c->is_object())
          fn(c.key(), *c, t.key(), *t);
    }
  }
}



// "Sound only" and "a picture of one frame" as jobs: nothing to encode, so one unit.
void run_extract(const std::shared_ptr<Job> &job, render::Composition comp, std::string path, bool sound, int64_t first, int64_t last, int width, int height);



// An image sequence: each frame of the range as a numbered PNG in `dir` (name_000000.png, numbered by the frame of the sequence). One thread renders
// and writes, a frame at a time; the job can be cancelled between frames.
// GPU devices that finished render jobs left behind, kept for the next job on the same device: starting one takes ~75 ms.
// A device is used by one job at a time (each job runs on its own thread); a job takes one from here or makes one, and
// gives it back when it ends (GpuLease).
struct GpuPool {
  std::mutex mutex;
  std::map<int, std::vector<std::unique_ptr<gpu::Context>>> spare;
};
struct GpuLease {
  std::shared_ptr<GpuPool> pool;
  int device = -1;
  std::unique_ptr<gpu::Context> ctx;
  gpu::Context *get() const { return ctx.get(); }
  GpuLease() = default;
  GpuLease(GpuLease &&) = default; // a moved-from lease holds nothing and gives nothing back
  GpuLease &operator=(GpuLease &&) = delete;
  GpuLease(const GpuLease &) = delete;
  ~GpuLease() {
    if (ctx && pool) {
      std::lock_guard lock(pool->mutex);
      pool->spare[device].push_back(std::move(ctx));
    }
  }
};
// The GPU a render job uses, or none: `device` is an index from gpu::list_devices, -1 for the CPU. Records on the job where
// its effects run, and why the GPU was not used when it could not be started.
GpuLease job_gpu(Job &job, int device, const std::shared_ptr<GpuPool> &pool);
void run_png_sequence(const std::shared_ptr<Job> &job, render::Composition comp, std::string dir, std::string name, int64_t first, int64_t last, int width, int height,
                      int gpu_device, std::shared_ptr<GpuPool> pool);



// The export runs as a pipeline: this thread renders frames into a few slots while "atm-encode" converts and
// encodes the previous ones, so the two never wait for each other. The encoder starts (hardware set-up, about half a
// second) and the audio is mixed while the first frames render.
void run_export(const std::shared_ptr<Job> &job, render::Composition comp, media::EncodeSettings settings, int64_t first, int64_t last, int gpu_device,
                std::shared_ptr<GpuPool> pool);

struct Engine::Impl {
  using Handler = Result<json> (Impl::*)(const json &);
  struct Tool {
    const char *name;
    const char *group;
    bool mutating;
    const char *summary;
    const char *params; // JSON Schema of the params object (MCP inputSchema); "" = takes none
    Handler fn;
  };
  static const Tool kTools[];
  static std::span<const Tool> tools(); // the table (engine_tools.cpp), with its size
  EngineConfig cfg;
  std::map<std::string, std::unique_ptr<Project>> projects;
 // by canonical path
  std::unordered_map<std::string, Project *> aliases;
       // the "project" strings callers used
  std::unordered_map<std::string_view, const Tool *> by_name;
  bool shutdown = false;
  Clock::time_point started = Clock::now();
  std::map<std::string, std::shared_ptr<Job>> jobs;
  std::vector<std::shared_ptr<gen::Provider>> providers;
  std::string comfyui_address;
  fs::path chosen_models_dir;
          // the user's own place for downloads; empty = the default one
  std::vector<fs::path> model_folders;
 // other folders that already hold model files; never written to
  std::shared_ptr<FinishedQueue> finished = std::make_shared<FinishedQueue>();
  ~Impl();
  Result<Project *> project(const json &params);
  static std::string key_of(const fs::path &dir);
  Result<Project *> open(const fs::path &raw);
  static Result<void> travel(Project &pr, int to);
  Result<void> recover(Project &pr, const std::vector<std::string> &records, const std::string &file_hash);
  Result<std::string> save(Project &pr);
  Result<void> journal_write(Project &pr, std::string_view record);
  void touched(Project &pr);
  Result<json> project_create(const json &params);
  Result<json> sequence_create(const json &params);
  Result<json> project_inspect(const json &params);
  Result<json> project_get(const json &params);
  Result<json> project_patch(const json &params);
  Result<json> step_history(Project &pr, int steps, bool undo);
  Result<json> project_undo(const json &params);
  Result<json> project_redo(const json &params);
  Result<json> project_validate(const json &params);
  Result<json> project_save(const json &params);
  Result<json> project_close(const json &params);
  Result<json> history_list(const json &params);
  Result<json> time_parse(const json &params);
  static const json &skills_of(const Project &pr);
  Result<json> fonts_list(const json &);
  fs::path library_dir() const;
  Result<fs::path> library_root() const;
  Result<json> library_add(const json &params);
  Result<json> library_read(const std::string &id) const;
  Result<json> library_insert(const json &params);
  Result<json> media_remove(const json &params);
  Result<json> audio_analyze(const json &params);
  Result<json> asr_transcribe(const json &params);
  Result<json> music_fit(const json &params);
  Result<json> clip_motion(const json &params);
  Result<json> text_pop(const json &params);
  Result<json> flash_cuts(const json &params);
  Result<json> music_cuts(const json &params);
  Result<json> audio_duck(const json &params);
  Result<json> sfx_make(const json &params);
  static std::string speech_node_of(const json &clip);
  Result<json> voice_make(const json &params);
  Result<json> voice_fit(const json &params);
  Result<json> script_plan(const json &params);
  Result<json> script_scenes(const json &params);
  Result<json> script_apply(const json &params);
  static std::vector<std::pair<std::string, std::string>> text_tracks_of(const json &sequence);
  Result<json> subtitles_export(const json &params);
  Result<json> subtitles_import(const json &params);
  // The rate of a sequence as frames a second (nominal, 30 for 29.97) and whether it is a drop-frame rate.
  struct EdlRate {
    Rational rate;
    int fps = 30;
    bool drop = false;
  };
  Result<EdlRate> edl_rate(const json &sequence, const json &params) const;
  static std::string edl_reel_of(const std::string &file_name);
  Result<json> edl_export(const json &params);
  Result<json> edl_import(const json &params);
  Result<json> library_list(const json &);
  Result<json> library_get(const json &params);
  Result<json> library_rename(const json &params);
  Result<json> library_remove(const json &params);
  Result<json> skill_list(const json &params);
  Result<json> skill_get(const json &params);
  Result<json> skill_save(const json &params);
  Result<json> skill_delete(const json &params);
  Result<json> guide_get(const json &params);
  Result<json> tools_list(const json &);
  Result<json> daemon_hello(const json &);
  Result<json> daemon_status(const json &);
  Result<json> daemon_shutdown(const json &);
  Result<json> media_probe(const json &params);
  Result<json> render_sequence(const json &params);
  // ---- see.* (agent feedback, MODULES §M12): rendered frames as JPEG files in <project>/.attome/see/ --------------

  struct Still {
    std::string path;
    int64_t frame = 0;
  };
  // The GPU of the see.* pictures (made on the engine's own thread), kept between calls; for the device chosen now.
  std::unique_ptr<gpu::Context> still_gpu;
  std::shared_ptr<GpuPool> gpu_pool = std::make_shared<GpuPool>(); // the render jobs' devices, kept between jobs
  int still_gpu_device = -2;
  gpu::Context *gpu_for_stills();
  // The GPU a render of `comp` uses: the device chosen now, or -1 (the CPU) when nothing in it has GPU work (effects, or
  // video clips, which the GPU decodes and draws), so starting a device costs nothing to a film of titles and stills.
  int gpu_device_for(const render::Composition &comp);
  Result<std::vector<std::vector<uint8_t>>> render_stills(render::Composition comp, const std::vector<int64_t> &frames,
                                                                 int width, int height, std::string *warning);
  Result<std::pair<render::Composition, fs::path>> see_setup(Project &pr, const json &params);
  static json time_of(int64_t frame, const render::Composition &comp);
  Result<json> see_frames(const json &params);
  Result<json> see_contact_sheet(const json &params);
  Result<json> media_import(const json &params);
  static void resolve_placeholders(json &v, const json &id_map);
  Result<json> timeline_edit(const json &params);
  bool model_installed(std::string_view id) const;
  static const json &library_of(const Project &pr);
  static const json &instance_of(const Project &pr, const std::string &clip_id);
  json model_warnings(const Project &pr, const std::string &clip_id) const;
  gen::KeyContext key_context(const Project &pr) const;
  static fs::path gen_dir(const Project &pr);
  static std::vector<gen::ClipIn> gen_clips(const Project &pr);
  std::vector<gen::ClipPlan> gen_plan(const Project &pr, gen::PlanOptions options) const;
  static json plan_json(const gen::ClipPlan &p);
  void engine_warnings(const Project &pr, const json &workflow, const std::string &owner, json &out, std::set<std::string> &seen) const;
  json ready_problems(const Project &pr, const std::string &clip_id) const;
  Result<json> gen_nodes(const json &);
  Result<json> gen_status(const json &params);
  Result<json> gen_run(const json &params);
  void length_ops(const Project &pr, const std::string &clip_id, const json &take, json &ops) const;
  void apply_finished();
  fs::path settings_path() const;
  json settings() const;
  // Where rendering runs (engine_device.cpp): the choice ("auto", "cpu" or a GPU's {name, vendor_id, device_id}), the GPUs here
  // (listed once, again on render.devices), and the timing checks of this run.
  json chosen_render_device;
  json device_checks = json::object();
  std::optional<std::vector<gpu::Device>> gpu_list;
  const std::vector<gpu::Device> &gpus(bool refresh = false);
  json render_choice() const;
  json device_check(const gpu::Device &d, bool fresh);
  json device_in_use(const std::vector<gpu::Device> &devices, const json &choice);
  json render_device_now(); // what renders now and why, for the answers of render.sequence and see.*
  Result<json> render_devices(const json &params);
  Result<json> render_set_device(const json &params);
  std::string saved_ffmpeg; // the FFmpeg path chosen this run (when there is no settings file to keep it in)
  Result<json> media_codecs(const json &params);
  void set_comfyui(const std::string &address);
  Result<json> gen_engines(const json &);
  Result<json> gen_set_comfyui(const json &params);
  Result<json> gen_models(const json &);
  Result<json> gen_create_clip(const json &params);
  Result<json> gen_save_to_library(const json &params);
  Result<json> gen_reset_clip(const json &params);
  Result<json> gen_save_preset(const json &params);
  Result<json> gen_apply_preset(const json &params);
  Result<json> gen_node_results(const json &params);
  Result<json> gen_export_workflow(const json &params);
  Result<json> gen_import_workflow(const json &params);
  Result<json> gen_select_take(const json &params);
  static bool models_dir_fixed();
  fs::path models_dir() const;
  static bool same_folder(const fs::path &a, const fs::path &b);
  bool knows_folder(const fs::path &folder) const;
  static int64_t free_bytes(const fs::path &dir);
  json folders_json() const;
  Result<void> save_model_folders();
  bool fetch_running() const;
  static int files_in(const std::vector<const models::CatalogFile *> &files, const fs::path &folder);
  Result<json> models_locate(const json &params);
  Result<json> models_forget_folder(const json &params);
  Result<json> models_set_folder(const json &params);
  Result<json> models_list(const json &);
  Result<json> models_fetch(const json &params);
  Result<std::shared_ptr<Job>> job_param(const json &params);
  Result<json> jobs_get(const json &params);
  Result<json> jobs_cancel(const json &params);
  Result<json> profile_get(const json &params);
  Result<json> profile_reset(const json &);
  Result<json> profile_set(const json &params);
};

} // namespace atm::api
