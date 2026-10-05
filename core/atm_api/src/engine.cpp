#include "atm/api/engine.hpp"
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
#include <set>
#include <thread>
#include <unordered_map>

#include "atm/base/hash.hpp"
#include "atm/base/id.hpp"
#include "atm/base/profiler.hpp"
#include "atm/base/time.hpp"
#include "atm/doc/document.hpp"
#include "atm/gen/library.hpp"
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
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using patch::ChangeSet;

fs::path to_path(const std::string &utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }

std::string to_utf8(const fs::path &p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}

int process_id() {
#if defined(_WIN32)
  return _getpid();
#else
  return int(getpid());
#endif
}

tl::unexpected<Error> bad_param(const char *key, const char *what) {
  return fail(ErrorCode::InvalidArgument, "E_PARAM", std::string("The parameter \"") + key + "\" " + what + ".", {},
              "See the parameters with: attome tools");
}

Result<const std::string *> string_param(const json &params, const char *key) {
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
  std::atomic<int> state{running};
  std::atomic<int64_t> units_done{0}, units_total{0};
  std::atomic<int64_t> fetched{0}; // a download: bytes that arrived in this run (units_done also counts what was on disk)
  std::atomic<bool> cancel{false};
  std::mutex mutex; // guards error, warning, seconds
  Error error;
  std::string warning;
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
void run_gen(const std::shared_ptr<Job> &job, GenRun run, std::shared_ptr<FinishedQueue> queue, std::string project) {
  prof::set_thread_name("atm-generate");
  GenProgress progress;
  progress.done = &job->units_done;
  progress.cancel = &job->cancel;
  progress.on_detail = [&](const std::string &text) {
    std::lock_guard lock(job->mutex);
    job->detail = text;
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

// The export runs as a pipeline: this thread renders frames into a few slots while "atm-encode" converts and
// encodes the previous ones, so the two never wait for each other. The encoder starts (hardware set-up, about half a
// second) and the audio is mixed while the first frames render.
void run_export(const std::shared_ptr<Job> &job, render::Composition comp, media::EncodeSettings settings) {
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
    auto encoder = media::Encoder::create(settings);
    if (mixer.joinable())
      mixer.join();
    if (!encoder)
      return stop(encoder.error());
    {
      std::lock_guard lock(job->mutex);
      job->encoder = (*encoder)->name();
    }
    size_t audio_pos = 0; // stereo frames already written
    const size_t audio_frames = audio.size() / 2;
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
      Result<void> r = (*encoder)->video(slots[size_t(index)].pixels.data(), f);
      {
        std::lock_guard lock(mutex);
        free_slots.push_back(index);
      }
      cv.notify_all();
      // Audio is interleaved about half a second at a time, so the muxer never holds much of one stream.
      const size_t audio_to =
          std::min(audio_frames, size_t(comp.frame_hns(f + 1) * media::kAudioRate / media::kHnsPerSecond));
      if (r && (audio_to - audio_pos >= size_t(media::kAudioRate / 2) || f + 1 == comp.frames) && audio_to > audio_pos) {
        r = (*encoder)->audio(audio.data() + audio_pos * 2, audio_to - audio_pos);
        audio_pos = audio_to;
      }
      if (!r)
        return stop(r.error());
      job->units_done.store(f + 1);
    }
    if (auto r = (*encoder)->finish(); !r)
      stop(r.error());
  });

  bool cancelled = false;
  for (int64_t f = 0; f < comp.frames; ++f) {
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

} // namespace

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

  EngineConfig cfg;
  std::map<std::string, std::unique_ptr<Project>> projects; // by canonical path
  std::unordered_map<std::string, Project *> aliases;       // the "project" strings callers used
  std::unordered_map<std::string_view, const Tool *> by_name;
  bool shutdown = false;
  Clock::time_point started = Clock::now();
  std::map<std::string, std::shared_ptr<Job>> jobs;
  std::vector<std::shared_ptr<gen::Provider>> providers;
  std::string comfyui_address;
  fs::path chosen_models_dir;          // the user's own place for downloads; empty = the default one
  std::vector<fs::path> model_folders; // other folders that already hold model files; never written to
  std::shared_ptr<FinishedQueue> finished = std::make_shared<FinishedQueue>();

  ~Impl() {
    for (auto &[id, job] : jobs) {
      job->cancel.store(true);
      if (job->thread.joinable())
        job->thread.join();
    }
  }

  // ---- projects -----------------------------------------------------------------------------------------------

  Result<Project *> project(const json &params) {
    ATM_TRY(const std::string *raw, string_param(params, "project"));
    if (const auto it = aliases.find(*raw); it != aliases.end())
      return it->second;
    if (id_prefix(*raw) == "prj") {
      for (auto &[key, pr] : projects)
        if (pr->doc.id() == *raw)
          return aliases[*raw] = pr.get();
      return fail(ErrorCode::NotFound, "R_NO_PROJECT", "No open project has the ID \"" + *raw + "\".", {},
                  "Pass the path of the .attome folder instead.");
    }
    ATM_TRY(Project *pr, open(to_path(*raw)));
    return aliases[*raw] = pr;
  }

  static std::string key_of(const fs::path &dir) {
    std::string key = to_utf8(dir);
#if defined(_WIN32) || defined(__APPLE__)
    for (char &c : key) // case-insensitive volumes
      if (c >= 'A' && c <= 'Z')
        c = char(c - 'A' + 'a');
#endif
    return key;
  }

  Result<Project *> open(const fs::path &raw) {
    ATM_PROFILE_SCOPE("project.open");
    std::error_code ec;
    fs::path dir = fs::weakly_canonical(fs::absolute(raw, ec), ec);
    if (ec)
      dir = raw;
    std::string key = key_of(dir);
    if (const auto it = projects.find(key); it != projects.end())
      return it->second.get();
    if (!storage::exists(dir / "project.json"))
      return fail(ErrorCode::NotFound, "R_NO_PROJECT", "\"" + to_utf8(dir) + "\" is not an Attome project.", {},
                  "Create one with: attome new <name>.attome");
    auto pr = std::make_unique<Project>();
    pr->dir = dir;
    pr->key = key;
    ATM_CHECK(storage::make_dirs(dir / ".attome" / "journal"));
    ATM_TRY(storage::File lock, storage::File::open(dir / ".attome" / "lock", storage::File::Mode::lock));
    pr->lock = std::move(lock);
    ATM_TRY(std::string text, storage::read_file(dir / "project.json"));
    ATM_TRY(doc::Document document, doc::Document::parse(text));
    pr->doc = std::move(document);
    std::vector<std::string> records;
    ATM_TRY(storage::RecordLog journal, storage::RecordLog::open(dir / ".attome" / "journal" / "000001.jnl", &records));
    pr->journal = std::move(journal);
    ATM_CHECK(recover(*pr, records, blake3_hex(text)));
    Project *out = pr.get();
    projects.emplace(std::move(key), std::move(pr));
    return out;
  }

  // Moves the document one step along the undo tree: to the parent of HEAD (undo) or to a child of HEAD (redo).
  static Result<void> travel(Project &pr, int to) {
    const int cur = pr.hist.head();
    const auto &nodes = pr.hist.nodes();
    const json *ops = nullptr;
    if (cur >= 0 && nodes[size_t(cur)].parent == to)
      ops = &nodes[size_t(cur)].inverse;
    else if (to >= 0 && nodes[size_t(to)].parent == cur)
      ops = &nodes[size_t(to)].forward;
    else
      return fail(ErrorCode::CorruptData, "R_HISTORY", "The history asks for a jump that is not one undo or redo step.");
    ATM_TRY(patch::ApplyResult applied, patch::apply(pr.doc, *ops, {.keep = true, .validate = false}));
    (void)applied;
    pr.hist.set_head(to);
    return {};
  }

  // Open flow of F0 §4.4: find the newest record whose file hash matches project.json, replay what came after it,
  // and rebuild the undo tree from every ChangeSet of the epoch.
  Result<void> recover(Project &pr, const std::vector<std::string> &records, const std::string &file_hash) {
    ATM_PROFILE_SCOPE("journal.replay");
    std::vector<json> recs;
    recs.reserve(records.size());
    int anchor = -1;
    for (const std::string &text : records) {
      json r = json::parse(text, nullptr, false);
      if (r.is_discarded() || !r.is_object())
        break;
      const std::string k = r.value("k", "");
      if ((k == "save" || k == "open") && r.value("file_hash", "") == file_hash)
        anchor = int(recs.size());
      recs.push_back(std::move(r));
    }
    if (anchor < 0) { // empty journal, or project.json was edited outside Attome: start a new epoch
      if (!records.empty()) {
        ATM_CHECK(pr.journal.clear());
        pr.new_epoch = true;
      }
      ATM_CHECK(pr.journal.append(json{{"k", "open"}, {"file_hash", file_hash}, {"engine", kEngineVersion}}.dump()));
      return pr.journal.sync();
    }
    for (int i = 0; i < int(recs.size()); ++i) {
      const json &r = recs[size_t(i)];
      const std::string k = r.value("k", "");
      const auto ref = [&](const char *field) {
        const auto it = r.find(field);
        return it != r.end() && it->is_string() ? pr.hist.find(it->get_ref<const std::string &>()) : -1;
      };
      if (k == "cs") {
        const int index = pr.hist.add(patch::History::from_record(r), ref("parent"));
        ++pr.revision;
        if (i > anchor) {
          auto applied = patch::apply(pr.doc, pr.hist.nodes()[size_t(index)].forward, {.keep = true, .validate = false});
          if (!applied)
            return fail(ErrorCode::CorruptData, "R_JOURNAL", "The journal does not fit project.json: " +
                                                                 applied.error().message,
                        {}, "Restore project.json from git, or delete .attome/journal to drop unsaved edits.");
          ++pr.recovered;
        }
      } else if (k == "head") {
        const int to = ref("to");
        if (i > anchor) {
          ATM_CHECK(travel(pr, to));
          ++pr.recovered;
        } else {
          pr.hist.set_head(to);
        }
        ++pr.revision;
      }
    }
    pr.dirty = pr.recovered > 0;
    pr.last_change = Clock::now();
    return {};
  }

  Result<std::string> save(Project &pr) {
    ATM_PROFILE_SCOPE("project.save");
    const std::string text = pr.doc.serialize();
    std::string hash;
    {
      ATM_PROFILE_SCOPE("hash.blake3");
      hash = blake3_hex(text);
    }
    // The save record is durable before the rename, so a crash in between replays from the older save.
    ATM_CHECK(pr.journal.append(json{{"k", "save"}, {"file_hash", hash}}.dump()));
    ATM_CHECK(pr.journal.sync());
    ATM_CHECK(storage::atomic_write(pr.dir / "project.json", text));
    pr.dirty = false;
    return hash;
  }

  Result<void> journal_write(Project &pr, std::string_view record) {
    ATM_PROFILE_SCOPE("journal.append");
    ATM_CHECK(pr.journal.append(record));
    if (cfg.fsync)
      ATM_CHECK(pr.journal.sync());
    return {};
  }

  void touched(Project &pr) {
    ++pr.revision;
    pr.dirty = true;
    pr.last_change = Clock::now();
  }

  // ---- Tools --------------------------------------------------------------------------------------------------

  Result<json> project_create(const json &params) {
    ATM_TRY(const std::string *raw, string_param(params, "path"));
    fs::path dir = fs::absolute(to_path(*raw));
    if (dir.extension() != ".attome")
      dir += ".attome";
    if (storage::exists(dir / "project.json"))
      return fail(ErrorCode::OutputExists, "R_EXISTS", "\"" + to_utf8(dir) + "\" already holds a project.", {},
                  "Choose another name, or open it with: attome inspect \"" + to_utf8(dir) + "\"");
    ATM_TRY(Rational rate, Rational::parse(params.value("rate", std::string("30"))));
    if (rate.num() <= 0)
      return bad_param("rate", "must be a positive frame rate such as \"30\" or \"30000/1001\"");
    int width = 1920, height = 1080;
    if (const auto canvas = params.find("canvas"); canvas != params.end() && canvas->is_object()) {
      width = canvas->value("width", width);
      height = canvas->value("height", height);
    }
    if (width < 16 || height < 16 || width > 32768 || height > 32768)
      return bad_param("canvas", "needs a width and height between 16 and 32768");
    const std::string name = params.value("name", to_utf8(dir.stem()));

    ATM_CHECK(storage::make_dirs(dir / ".attome" / "journal"));
    ATM_TRY(doc::Document document, doc::Document::from_json(doc::new_project(name, rate, width, height)));
    ATM_CHECK(storage::atomic_write(dir / "project.json", document.serialize()));
    ATM_CHECK(storage::atomic_write(dir / ".gitignore", ".attome/\n"));
    ATM_CHECK(storage::atomic_write(dir / ".gitattributes", "*.json text eol=lf\n"));
    ATM_TRY(Project *pr, open(dir));
    return json{{"project", pr->doc.id()},
                {"path", to_utf8(pr->dir)},
                {"sequence", pr->doc.root()["sequence_order"][0]},
                {"revision", pr->revision}};
  }

  Result<json> project_inspect(const json &params) {
    ATM_TRY(Project *pr, project(params));
    const std::string level = params.value("level", std::string("summary"));
    const size_t max_items = params.value("max_items", size_t(200));
    const json &root = pr->doc.root();
    json sequences = json::array();
    std::string text = "Project \"" + root.value("name", "") + "\"  " + pr->doc.id() + "  revision " +
                       std::to_string(pr->revision) + "  " + std::to_string(pr->doc.object_count()) + " objects\n";
    const auto order_of = [](const json &owner, const char *key) -> const json & {
      static const json empty = json::array();
      const auto it = owner.find(key);
      return it != owner.end() && it->is_array() ? *it : empty;
    };
    for (const json &seq_id : order_of(root, "sequence_order")) {
      const json &seq = root["sequences"][seq_id.get_ref<const std::string &>()];
      json tracks = json::array();
      text += "  Sequence \"" + seq.value("name", "") + "\"  " + seq_id.get<std::string>() + "  " +
              seq.value("rate", "") + " fps\n";
      for (const json &trk_id : order_of(seq, "track_order")) {
        const json &trk = seq["tracks"][trk_id.get_ref<const std::string &>()];
        const json &clip_order = order_of(trk, "clip_order");
        json track = {{"id", trk_id}, {"name", trk.value("name", "")}, {"kind", trk.value("kind", "")},
                      {"clips", clip_order.size()}};
        text += "    Track \"" + trk.value("name", "") + "\" (" + trk.value("kind", "") + ")  " +
                trk_id.get<std::string>() + "  " + std::to_string(clip_order.size()) + " clips\n";
        if (level != "summary") {
          json clips = json::array();
          for (const json &clip_id : clip_order) {
            if (clips.size() >= max_items) {
              text += "      … " + std::to_string(clip_order.size() - clips.size()) + " more\n";
              break;
            }
            const json &clip = trk["clips"][clip_id.get_ref<const std::string &>()];
            const json timing = clip.value("timing", json::object());
            const std::string in = timing.value("record_in", "?"), dur = timing.value("duration", "?");
            const std::string type = clip.value("media_ref", json::object()).value("type", "");
            clips.push_back({{"id", clip_id}, {"name", clip.value("name", "")}, {"record_in", in},
                             {"duration", dur}, {"type", type}});
            text += "      " + clip_id.get<std::string>() + "  in " + in + "  dur " + dur + "  " + type + "  " +
                    clip.value("name", "") + "\n";
          }
          track["clip_list"] = std::move(clips);
        }
        tracks.push_back(std::move(track));
      }
      sequences.push_back({{"id", seq_id}, {"name", seq.value("name", "")}, {"rate", seq.value("rate", "")},
                           {"canvas", seq.value("canvas", json::object())}, {"tracks", std::move(tracks)}});
    }
    if (pr->recovered)
      text += "  Recovered " + std::to_string(pr->recovered) + " unsaved edit(s) from the journal.\n";
    json data = {{"id", pr->doc.id()},          {"name", root.value("name", "")}, {"path", to_utf8(pr->dir)},
                 {"objects", pr->doc.object_count()}, {"unsaved", pr->dirty},      {"recovered", pr->recovered},
                 {"sequences", std::move(sequences)}};
    return json{{"text", std::move(text)}, {"data", std::move(data)}, {"revision", pr->revision}};
  }

  Result<json> project_get(const json &params) {
    ATM_TRY(Project *pr, project(params));
    ATM_TRY(const std::string *id, string_param(params, "id"));
    const doc::NodeRef *ref = pr->doc.find(*id);
    if (!ref)
      return fail(ErrorCode::UnknownId, "P_UNKNOWN_ID", "No object has the ID \"" + *id + "\".", *id,
                  "List the project's IDs with: attome inspect <project> --level full");
    json out = json::object();
    out["object"] = *ref->node;
    out["parent"] = ref->parent;
    out["revision"] = pr->revision;
    return out;
  }

  Result<json> project_patch(const json &params) {
    ATM_TRY(Project *pr, project(params));
    const auto pit = params.find("patch");
    if (pit == params.end() || !(pit->is_object() || pit->is_array()))
      return bad_param("patch", "is required: {\"ops\": [...]} or a bare array of ops");
    static const json no_ops;
    const json &ops = pit->is_array() ? *pit : (pit->contains("ops") ? (*pit)["ops"] : no_ops);
    const bool dry_run = params.value("dry_run", false);
    if (pit->is_object())
      if (const auto base = pit->find("base_revision"); base != pit->end() && base->is_number_unsigned() &&
                                                        base->get<uint64_t>() != pr->revision) {
        Error e;
        e.code = ErrorCode::StaleBaseRevision;
        e.rule = "R_STALE_REVISION";
        e.message = "The project is at revision " + std::to_string(pr->revision) + ", not " +
                    std::to_string(base->get<uint64_t>()) + ".";
        e.hint = "Read the project again and rebuild the patch.";
        e.details = {{"revision", pr->revision}};
        return tl::unexpected(std::move(e));
      }

    ATM_TRY(patch::ApplyResult res, patch::apply(pr->doc, ops, {.keep = !dry_run, .validate = true}));

    json out = json::object();
    out["applied"] = !dry_run;
    out["id_map"] = std::move(res.id_map);
    out["impact"] = {{"created", res.created},
                     {"deleted", res.deleted},
                     {"modified", res.modified},
                     {"dirty", nullptr},     // M6 fills this from F2
                     {"estimate", nullptr}}; // M17 fills this from F2
    if (dry_run) {
      out["patch"]["ops"] = std::move(res.ops);
      out["inverse"]["ops"] = std::move(res.inverse);
      out["revision"] = pr->revision;
      return out;
    }

    ATM_PROFILE_SCOPE("history.commit");
    ChangeSet cs;
    cs.id = new_id("cs");
    cs.time_utc = utc_now_iso8601();
    if (pit->is_object()) {
      cs.task = pit->value("task", "");
      cs.label = pit->value("label", "");
    }
    cs.task = params.value("task_id", cs.task);
    cs.forward = std::move(res.ops);
    cs.inverse = std::move(res.inverse);
    const int parent = pr->hist.head();
    const std::string record =
        patch::History::to_record(cs, parent < 0 ? std::string_view{} : pr->hist.nodes()[size_t(parent)].id);
    if (auto written = journal_write(*pr, record); !written) { // not durable: take the edit back
      (void)patch::apply(pr->doc, cs.inverse, {.keep = true, .validate = false});
      return tl::unexpected(std::move(written.error()));
    }
    out["changeset"] = cs.id;
    pr->hist.add(std::move(cs), parent);
    touched(*pr);
    out["revision"] = pr->revision;
    return out;
  }

  Result<json> step_history(Project &pr, int steps, bool undo) {
    json moved = json::array();
    for (int i = 0; i < steps; ++i) {
      const int cur = pr.hist.head();
      int to = -1;
      if (undo) {
        if (cur < 0)
          break;
        to = pr.hist.nodes()[size_t(cur)].parent;
      } else {
        const auto &next = pr.hist.children_of(cur);
        if (next.empty())
          break;
        to = next.back(); // the most recent branch
      }
      ATM_CHECK(travel(pr, to));
      const auto &nodes = pr.hist.nodes();
      moved.push_back(nodes[size_t(undo ? cur : to)].id);
      const json record = {{"k", "head"}, {"to", to < 0 ? json(nullptr) : json(nodes[size_t(to)].id)}};
      ATM_CHECK(journal_write(pr, record.dump()));
      touched(pr);
    }
    if (moved.empty())
      return fail(ErrorCode::NotFound, undo ? "R_HISTORY_START" : "R_HISTORY_END",
                  undo ? "There is nothing to undo." : "There is nothing to redo.");
    const int head = pr.hist.head();
    return json{{undo ? "undone" : "redone", std::move(moved)},
                {"head", head < 0 ? json(nullptr) : json(pr.hist.nodes()[size_t(head)].id)},
                {"revision", pr.revision}};
  }

  Result<json> project_undo(const json &params) {
    ATM_TRY(Project *pr, project(params));
    return step_history(*pr, std::max(1, params.value("steps", 1)), true);
  }

  Result<json> project_redo(const json &params) {
    ATM_TRY(Project *pr, project(params));
    return step_history(*pr, std::max(1, params.value("steps", 1)), false);
  }

  Result<json> project_validate(const json &params) {
    ATM_TRY(Project *pr, project(params));
    json errors = patch::validate_document(pr->doc);
    const bool ok = errors.empty();
    // Not errors: the project is valid, but a workflow cannot run here until its model is chosen or installed.
    json warnings = json::array();
    for (const gen::ClipIn &clip : gen_clips(*pr))
      for (json &w : ready_problems(*pr, clip.id))
        warnings.push_back(std::move(w));
    return json{{"ok", ok}, {"errors", std::move(errors)}, {"warnings", std::move(warnings)}, {"revision", pr->revision}};
  }

  Result<json> project_save(const json &params) {
    ATM_TRY(Project *pr, project(params));
    ATM_TRY(std::string hash, save(*pr));
    return json{{"revision", pr->revision}, {"file_hash", std::move(hash)}};
  }

  Result<json> project_close(const json &params) {
    ATM_TRY(Project *pr, project(params));
    if (pr->dirty)
      ATM_CHECK(save(*pr).map([](const std::string &) {}));
    const uint64_t revision = pr->revision;
    std::erase_if(aliases, [&](const auto &entry) { return entry.second == pr; });
    projects.erase(pr->key);
    return json{{"closed", true}, {"revision", revision}};
  }

  Result<json> history_list(const json &params) {
    ATM_TRY(Project *pr, project(params));
    const size_t limit = params.value("limit", size_t(50));
    const auto &nodes = pr->hist.nodes();
    json list = json::array();
    for (size_t i = nodes.size() > limit ? nodes.size() - limit : 0; i < nodes.size(); ++i) {
      const ChangeSet &cs = nodes[i];
      list.push_back({{"id", cs.id},
                      {"parent", cs.parent < 0 ? json(nullptr) : json(nodes[size_t(cs.parent)].id)},
                      {"seq", cs.seq},
                      {"task", cs.task},
                      {"label", cs.label},
                      {"time", cs.time_utc},
                      {"ops", cs.forward.size()}});
    }
    const int head = pr->hist.head();
    return json{{"head", head < 0 ? json(nullptr) : json(nodes[size_t(head)].id)},
                {"changesets", std::move(list)},
                {"revision", pr->revision}};
  }

  Result<json> time_parse(const json &params) {
    const auto value = params.find("value");
    if (value == params.end())
      return bad_param("value", "is required");
    TimeContext ctx;
    if (const auto rate = params.find("rate"); rate != params.end() && rate->is_string()) {
      ATM_TRY(Rational r, Rational::parse(rate->get_ref<const std::string &>()));
      ctx.rate = r;
    }
    ATM_TRY(RationalTime t, parse_time(*value, ctx));
    return to_json(format_time(t, ctx.rate));
  }

  // Recipes for agents. Tool descriptions are short because MCP clients cut long ones; the details live here.
  Result<json> guide_get(const json &params) {
    static const std::pair<const char *, const char *> kTopics[] = {
        {"timeline",
         "timeline.edit is the easy way to edit: {\"project\":…,\"ops\":[…]} applies all ops as one step. Give an op "
         "\"id\":\"$new:name\" and later ops use that name. Times: \"2.5s\", \"75@30\", timecode.\n"
         "- add_clip {asset (from media.import) or path (a video, a sound file, or a PNG / JPEG picture, which lasts 5 s "
         "unless given a duration and keeps its transparency), track? (ID, $new name or \"new\"; default the bottom video "
         "track, or an audio track for sound files), at? (default: after the last clip of the track), source_in? "
         "(default 0), duration? (default the rest of the file), with_audio? (false: picture only), volume?, gain_db?, "
         "position?, scale?, rotation?, anchor?, crop?, opacity?, fade_in?, fade_out?, audio_track?}. A video with sound becomes two LINKED "
         "clips: the picture, and its sound on an audio track (named \"$new:<name>.audio\" in id_map). Edits (move, "
         "trim, split, delete, slip, roll, slide) apply to both; add \"unlink\": true to an op to edit one alone (for "
         "J and L cuts). A dissolve between two linked clips also cross-fades their sound.\n"
         "- add_text {text (\\n for a new line), at? (0), duration? (3s), placement? (center | lower_third | top | "
         "bottom) or position?, size? (0.08 of the height), color? (#ffffff), bold? (true), rotation?, fade_in?, "
         "fade_out?} - "
         "goes on a Titles track on top\n"
         "- add_adjustment {at?, duration? (2s), blur? (radius, e.g. 0.02) or effects? [{type, ...params}], opacity?, fade_in?, fade_out?} - changes "
         "everything below; goes on an Effects track under the titles\n"
         "- add_transition {between: [first, second], type? (dissolve | wipe | push | zoom | slide | iris), direction? (left | right | up | down, for a wipe, push or slide: the side the new clip enters from; in | out for a zoom), softness? (0.01..1, the soft edge of a wipe (0.1) or an iris (0.15)), amount? (0.05..2, a zoom's size, 0.5), duration? (1s), alignment? (center | start | end), make_room? (true: trim what the media lacks, see make_room)} - a dissolve, a wipe (an edge crosses the picture), a push (the old clip slides away, the new one follows) a zoom (in: the old picture grows while the new one settles into place; out: the old one shrinks away over the new one), a slide (the new clip comes in over the old one, which stays where it is) or an iris (the new clip opens as a circle from the centre); "
         "the clips must touch, and center needs half the duration of spare media on each side of the cut\n"
         "- make_room {between: [first, second], duration? (1s), alignment? (center), ripple? (\"synced\" (default) | \"track\" | \"all\" | [track IDs])} - trims what a transition of that length "
         "would lack: the end of the first clip and the start of the second come in by the missing media, the second clip moves up "
         "to meet the first, and every later clip of its track (and the clips linked to them) moves up by the same amount, so no "
         "gap opens and the track gets shorter after the cut. Clips on other tracks that are not linked stay put unless the track "
         "is locked to the cut (\"sync_lock\": true on the track; the Titles and Effects tracks timeline.edit makes are) or ripple "
         "names it (\"all\" or a list of IDs; \"track\" keeps every other track still): their clips after the cut move up, titles and effect layers that span it "
         "get shorter, one that starts inside the trimmed part loses its head, and a clip with media that spans it is left alone "
         "with a note. add_transition with \"make_room\": true (and \"ripple\") does both in one step\n"
         "- add_track {kind (video | audio), name?, position? (top | bottom), below? / above? (track ID), sync_lock? (true: the track's clips follow make_room and ripple_delete)}\n"
         "- delete {clip} or {transition}; ripple_delete {clip} (closes the gap, on the tracks locked to the cut too); move {clip, to?, track?}; trim {clip, edge (in | out), to or delta}; "
         "split {clip, at}; slip {clip, delta} (shows another part of its file, stays in place); roll {between: "
         "[first, second], delta} (moves the cut between them); slide {clip, delta} (moves the clip between its "
         "neighbours, which give and take the time)\n"
         "- add_effect {target (clip), type? (gaussian_blur | color_grade | vignette | sharpen | film_grain | lut | chroma_key | luma_key), plus the effect's parameters: radius | brightness, contrast, saturation | strength, radius, softness | amount, radius | strength, size | file, strength | hue, similarity, smoothness, detail | level, tolerance, softness} changes only that clip (a blur softens its edges "
         "into what is below; remove_effect {effect}; set_effect_enabled {effect, enabled}\n"
         "- link {clips: [...]} joins clips so edits move them together; unlink {clip} takes one out of its group\n"
         "- set_property {target (clip, track or fx ID), path (e.g. \"audio.gain_db\", \"transform.opacity\", "
         "\"content.text\", \"volume\", \"params.radius\", \"transform.crop\"), value} or {target, path "
         "\"transform.opacity|position|scale|rotation|anchor\", keyframes: [{t, v, interp?, ease?}]} or, for an effect, "
         "{target (fx ID), path \"params.<name>\", keyframes: [...]} to animate that parameter\n"
         "Example: four clips with dissolves, a fading title, a blur and music:\n"
         "[{\"op\":\"add_clip\",\"id\":\"$new:a\",\"asset\":\"ast_…\",\"source_in\":\"1s\",\"duration\":\"4s\",\"with_audio\":false},"
         "{\"op\":\"add_clip\",\"id\":\"$new:b\",\"asset\":\"ast_…\",\"source_in\":\"1s\",\"duration\":\"4s\",\"with_audio\":false},"
         "{\"op\":\"add_transition\",\"between\":[\"$new:a\",\"$new:b\"],\"duration\":\"1s\"},"
         "{\"op\":\"add_text\",\"text\":\"Summer in the City\\nصيف في المدينة\",\"placement\":\"lower_third\",\"duration\":\"4s\","
         "\"fade_in\":\"0.5s\",\"fade_out\":\"0.5s\"},"
         "{\"op\":\"add_adjustment\",\"duration\":\"2s\",\"blur\":0.02,\"fade_out\":\"1s\"},"
         "{\"op\":\"add_clip\",\"asset\":\"ast_music…\",\"at\":\"0s\",\"duration\":\"8s\",\"gain_db\":-12,\"fade_out\":\"2s\"}]\n"
         "Leave handles for dissolves: start clips a little into their files (source_in) and do not use them to "
         "their very end. The result has id_map ($new names -> IDs), notes and the new duration."},
        {"clips",
         "Tracks and clips as raw project.patch ops (timeline.edit builds these for you).\n"
         "Add a track: {\"op\":\"add\",\"path\":\"<seq_id>/tracks/$new:v1\",\"value\":{\"kind\":\"video\",\"name\":\"V1\"}} "
         "(kind \"audio\" for sound only). Tracks stack in order: the first is the bottom layer. The sequence ID comes "
         "from project.create or project.inspect.\n"
         "Add a clip from a file: {\"op\":\"add\",\"path\":\"<track_id or $new:v1>/clips/$new:c1\",\"value\":{\"name\":"
         "\"beach\",\"timing\":{\"record_in\":\"0s\",\"duration\":\"4s\",\"source_in\":\"1s\"},\"media_ref\":{\"type\":"
         "\"file\",\"path\":\"C:/media/beach.mp4\",\"duration\":\"<file duration from media.probe>\"},\"transform\":"
         "{\"position\":[0.5,0.5],\"scale\":[1,1],\"opacity\":1},\"volume\":1}}\n"
         "record_in = where the clip starts on the timeline; duration = how long it plays; source_in = where it starts "
         "in the file. Clips on one track may not overlap. position is where the clip's anchor sits, in canvas "
         "fractions from the top-left ([0.5,0.5] = centre); scale 1 fits the canvas; opacity 0..1; volume 0 mutes the "
         "clip's sound, 1 leaves it unchanged. Always give media_ref.duration: dissolves need it to check the media.\n"
         "Optional: \"rotation\" in degrees, clockwise (90 stands a sideways phone video up); \"anchor\" [x, y], the "
         "point of the clip's own picture that position places and that scale and rotation turn around (default "
         "[0.5,0.5], its centre; [0,0] is its top-left corner); \"crop\" {\"left\":0.1,\"top\":0,\"right\":0.1,"
         "\"bottom\":0}, fractions of the picture cut off each side (the rest stays in place)."},
        {"text",
         "Text clips (titles, lower thirds, captions) have no file: \"media_ref\":{\"type\":\"text\"},\"content\":"
         "{\"text\":\"Summer in the City\\nصيف في المدينة\",\"size\":0.08,\"color\":\"#ffffff\",\"bold\":true}. size is "
         "the font height as a fraction of the canvas height; \\n starts a new line; lines are centred. Arabic and "
         "other right-to-left text is shaped correctly. Put text on its own track after (above) the video tracks. "
         "A lower third: \"transform\":{\"position\":[0.5,0.84]}. To fade it, see the topic \"keyframes\"."},
        {"dissolves",
         "A dissolve mixes two clips that touch on one track (the first ends exactly where the second starts):\n"
         "{\"op\":\"add\",\"path\":\"<track_id>/transitions/$new:d1\",\"value\":{\"type\":\"attome.dissolve\",\"from\":"
         "\"<first clip>\",\"to\":\"<second clip>\",\"in_offset\":\"0.5s\",\"out_offset\":\"0.5s\"}}\n"
         "The mix runs from cut - in_offset to cut + out_offset, and uses media beyond the cut: the first clip's file "
         "must go on for out_offset past its end (source_in + duration + out_offset <= file duration), and the "
         "second clip must have source_in >= in_offset. So leave handles: do not use a file up to its very end, and "
         "start the next clip a little into its file. The sound cross-fades with the picture. A refused dissolve "
         "names the largest offsets that fit; timeline.edit make_room (or add_transition with make_room true) trims and moves "
         "up what is missing instead. To fade one clip to or from black, use opacity keyframes instead."},
        {"keyframes",
         "Animate opacity, position, scale, rotation or anchor with keyframes inside the clip's transform (crop stays "
         "fixed). t is the time from the "
         "clip's start. Fade a 4-second title in over 0.5 s and out over its last 0.5 s:\n"
         "\"transform\":{\"position\":[0.5,0.84],\"opacity\":1,\"keyframes\":{\"opacity\":{"
         "\"$new:k1\":{\"t\":\"0s\",\"v\":0},\"$new:k2\":{\"t\":\"0.5s\",\"v\":1},"
         "\"$new:k3\":{\"t\":\"3.5s\",\"v\":1},\"$new:k4\":{\"t\":\"4s\",\"v\":0}}}}\n"
         "Each property is a map of keys {t, v}; give every key its own $new: name. v is a number for opacity and "
         "rotation (degrees) and [x, y] for position, scale and anchor. Before the first key the value is the first key's, after the last the last "
         "key's. A segment uses the interp of its left key: \"linear\" (default), \"hold\", or \"easing\" with "
         "\"ease\": ease_in_quad, ease_out_quad, ease_in_out_quad, ease_in_cubic, ease_out_cubic, ease_in_out_cubic, "
         "ease_in_expo, ease_out_expo, ease_in_out_expo or ease_out_back.\n"
         "Add one key to a clip that exists: {\"op\":\"add\",\"path\":\"<clip_id>/transform/keyframes/opacity/$new:k5\","
         "\"value\":{\"t\":\"2s\",\"v\":0.5}}. Keyframes replace the plain value while they exist.\n"
         "Effect parameters animate the same way, with the keys inside the effect object (t is clip-local, v a number "
         "inside the parameter's range): {\"op\":\"add\",\"path\":\"<fx_id>/keyframes/radius/$new:k1\",\"value\":{\"t\":"
         "\"0s\",\"v\":0.1,\"interp\":\"easing\",\"ease\":\"ease_out_cubic\"}} - or timeline.edit set_property with the fx ID, "
         "path \"params.radius\" and keyframes. A blur that clears over 2 s, or a vignette that closes in, needs only two keys."},
        {"effects",
         "Effects go on an adjustment layer: a clip with no picture of its own that changes everything on the tracks "
         "below it while it plays. The effects: attome.gaussian_blur (radius), attome.color_grade (brightness -1..1, contrast -1..1, saturation 0..3, 1 = unchanged) attome.vignette (strength 0..1, radius 0..1 where darkening starts, softness 0.01..1), attome.sharpen (amount 0..4, radius as a blur's) attome.film_grain (strength 0..1, size 1..8 pixels; the noise is new every frame), attome.chroma_key (a clip only, not an adjustment layer: makes the colour near hue transparent so the tracks below show; hue 0..360 degrees, 120 green, 240 blue; similarity 0..1 how far in hue from that colour is still removed (default 0.25; lower it if golden fur or similar colours near the screen hue are being lost, raise it if the screen drifts in colour); smoothness 0..1 the soft edge; detail 0..1, default 1, keeps thin lines such as hair inside the keyed-out area, and a tracking marker with them: lower values keep only the strongest lines, 0 switches that pass off (the colour match alone: faster, and it keeps less of fine hair and of a dark tracking marker); brightness does not matter, so a shadow on the screen goes too), attome.luma_key (a clip only: makes the brightness near level transparent, 0 black .. 1 white, for a title on black or a logo on white; tolerance 0..1 how far from that brightness is still removed; softness 0..1 the soft edge) and attome.lut (params.file: the path of a .cube colour table, 3D or 1D, as every grading tool exports; strength 0..1 mixes it with the unchanged picture; a file that is missing or unreadable is a warning and leaves the picture as it is; change the file with set_property on the effect, path \"params.file\"). Blur the video under a title for its "
         "first 2 s and let the blur fade out: put a track between the video and the title tracks (tracks stack in "
         "order): {\"op\":\"add\",\"path\":\"<seq_id>/tracks/$new:fx\",\"anchor\":{\"before\":\"<title track id>\"},"
         "\"value\":{\"kind\":\"video\",\"name\":\"Effects\"}} (an anchor of first, last, before or after places a new "
         "track), then\n"
         "{\"op\":\"add\",\"path\":\"<track_id>/clips/$new:adj\",\"value\":{\"name\":\"blur\",\"timing\":{\"record_in\":"
         "\"0s\",\"duration\":\"2s\",\"source_in\":\"0s\"},\"media_ref\":{\"type\":\"adjustment\"},\"effects\":{"
         "\"$new:fx1\":{\"effect\":\"attome.gaussian_blur@1.0.0\",\"enabled\":true,\"params\":{\"radius\":0.02}}},"
         "\"transform\":{\"opacity\":1,\"keyframes\":{\"opacity\":{\"$new:b1\":{\"t\":\"1s\",\"v\":1},\"$new:b2\":{\"t\":"
         "\"2s\",\"v\":0}}}}}}\n"
         "radius is a fraction of the picture height (0.02 soft, 0.1 strong, at most 0.25). The layer's opacity mixes "
         "the blurred picture with the sharp one, so opacity keyframes fade the effect in or out.\n"
         "To blur one clip only, put the effect on that clip instead: {\"op\":\"add\",\"path\":\"<clip_id>/effects/"
         "$new:fx\",\"value\":{\"effect\":\"attome.gaussian_blur@1.0.0\",\"enabled\":true,\"params\":{\"radius\":"
         "0.02}}} (or timeline.edit add_effect). Its edges soften into what is below it."},
        {"audio",
         "Sound. Music or a voice-over is a clip on an audio track (kind \"audio\"); files without video (mp3, wav, "
         "m4a) belong there. Video clips play their own sound too.\n"
         "Music under everything at -12 dB with a 2 s fade-out:\n"
         "{\"op\":\"add\",\"path\":\"<seq_id>/tracks/$new:a1\",\"value\":{\"kind\":\"audio\",\"name\":\"Music\"}}, then "
         "{\"op\":\"add\",\"path\":\"$new:a1/clips/$new:m\",\"value\":{\"name\":\"music\",\"timing\":{\"record_in\":"
         "\"0s\",\"duration\":\"20s\",\"source_in\":\"0s\"},\"media_ref\":{\"type\":\"file\",\"path\":\"C:/media/"
         "song.mp3\",\"duration\":\"<from media.probe>\"},\"audio\":{\"gain_db\":-12,\"fade_in\":\"0s\",\"fade_out\":"
         "\"2s\"}}}\n"
         "Clip \"audio\": gain_db (-96 to 24; -12 is about a quarter of the level), pan (-1 left .. 1 right), fade_in "
         "and fade_out (times from the clip's ends; together at most its duration), fade_curve \"equal_power\" "
         "(default) or \"linear\". A video added with timeline.edit has its sound as a linked clip on an audio track: "
         "mute it by setting that clip's volume to 0, or add the video with with_audio:false. An older clip that "
         "still carries its own sound is muted with \"volume\":0. A track can carry "
         "volume_db and pan for all its clips. Sound under a dissolve cross-fades by itself."},
        {"times",
         "Times accept \"12.5s\", \"375@30\" (frames at a rate), SMPTE \"00:00:12:15\" (needs the sequence rate) or "
         "{\"num\":25,\"den\":2} seconds. They are stored as exact rationals of seconds, such as \"25/2\". Cuts between "
         "two frames are allowed; rendering rounds to the nearest frame."},
    };
    const std::string want = params.value("topic", std::string());
    std::string text;
    for (const auto &[name, body] : kTopics)
      if (want.empty() || want == name)
        text += std::string("## ") + name + "\n" + body + "\n\n";
    if (text.empty())
      return bad_param("topic", "must be one of timeline, clips, text, dissolves, keyframes, effects, audio, times");
    return json{{"text", std::move(text)}};
  }

  Result<json> tools_list(const json &); // defined after the table

  Result<json> daemon_hello(const json &) {
    return json{{"protocol", kProtocolVersion},
                {"engine", kEngineVersion},
                {"schema", json::array({"1.0.0"})},
                {"pid", process_id()}};
  }

  Result<json> daemon_status(const json &) {
    json open = json::array();
    for (const auto &[key, pr] : projects)
      open.push_back({{"id", pr->doc.id()}, {"path", to_utf8(pr->dir)}, {"revision", pr->revision},
                      {"objects", pr->doc.object_count()}, {"unsaved", pr->dirty}});
    return json{{"engine", kEngineVersion},
                {"pid", process_id()},
                {"uptime_s", std::chrono::duration<double>(Clock::now() - started).count()},
                {"fsync", cfg.fsync},
                {"profiling", prof::enabled()},
                {"projects", std::move(open)}};
  }

  Result<json> daemon_shutdown(const json &) {
    shutdown = true;
    return json{{"stopping", true}};
  }

  Result<json> media_probe(const json &params) {
    ATM_TRY(const std::string *path, string_param(params, "path"));
    ATM_TRY(media::MediaInfo info, media::probe(*path));
    if (info.is_image) // a still: any length on the timeline
      return json{{"path", *path},           {"has_video", true},      {"has_audio", false},
                  {"image", true},           {"width", info.width},    {"height", info.height}};
    ATM_TRY(Rational duration, Rational::make(info.duration_hns, media::kHnsPerSecond));
    json out = {{"path", *path},           {"has_video", info.has_video}, {"has_audio", info.has_audio},
                {"duration", duration.to_string()}, {"seconds", duration.to_seconds_lossy()}};
    if (info.has_video) {
      ATM_TRY(Rational rate, Rational::make(info.rate_num, info.rate_den));
      out["width"] = info.width;
      out["height"] = info.height;
      out["rate"] = rate.to_string();
    }
    if (info.has_audio) {
      out["audio_rate"] = info.audio_rate;
      out["audio_channels"] = info.audio_channels;
    }
    return out;
  }

  Result<json> render_sequence(const json &params) {
    ATM_TRY(Project *pr, project(params));
    ATM_TRY(const std::string *output, string_param(params, "output"));
    ATM_TRY(render::Composition comp, render::compile(pr->doc.root(), params.value("sequence", std::string()), to_utf8(pr->dir)));
    if (comp.frames <= 0)
      return fail(ErrorCode::InvalidArgument, "R_EMPTY", "The sequence has no media clips to render.", {},
                  "Add a clip whose media_ref is {\"type\": \"file\", \"path\": …} first.");
    fs::path out_path = fs::absolute(to_path(*output));
    if (out_path.extension() != ".mp4")
      out_path += ".mp4";
    if (!params.value("overwrite", true) && storage::exists(out_path))
      return fail(ErrorCode::OutputExists, "R_EXISTS", "\"" + to_utf8(out_path) + "\" already exists.", {},
                  "Pass \"overwrite\": true or choose another name.");
    ATM_CHECK(storage::make_dirs(out_path.parent_path()));

    media::EncodeSettings settings;
    settings.path = to_utf8(out_path);
    settings.height = params.value("height", comp.height);
    settings.width = params.contains("width")
                         ? params.value("width", comp.width)
                         : int(int64_t(comp.width) * settings.height / std::max(1, comp.height));
    settings.rate_num = comp.rate_num;
    settings.rate_den = comp.rate_den;
    settings.audio = params.value("audio", true);
    // About 12 Mbit/s at 1080p30, scaled with the picture size.
    settings.bitrate = params.value("bitrate", std::max(1'000'000, settings.width * settings.height * 6));
    if (settings.width < 16 || settings.height < 16 || settings.width > 4096 || settings.height > 2304)
      return bad_param("height", "gives a size outside 16 x 16 … 4096 x 2304 (the H.264 encoder's range)");

    auto job = std::make_shared<Job>();
    job->id = new_id("job");
    job->kind = "render.sequence";
    job->output = settings.path;
    job->units_total.store(comp.frames);
    jobs[job->id] = job;
    job->thread = std::thread(run_export, job, std::move(comp), settings);
    return json{{"job_id", job->id}, {"output", job->output}, {"frames", job->units_total.load()},
                {"width", settings.width & ~1}, {"height", settings.height & ~1}};
  }

  // ---- see.* (agent feedback, MODULES §M12): rendered frames as JPEG files in <project>/.attome/see/ --------------

  struct Still {
    std::string path;
    int64_t frame = 0;
  };

  // Renders `frames` at width x height as packed BGRX pictures.
  static Result<std::vector<std::vector<uint8_t>>> render_stills(render::Composition comp, const std::vector<int64_t> &frames,
                                                                 int width, int height, std::string *warning) {
    render::Renderer renderer(std::move(comp), width, height);
    std::vector<uint8_t> nv12(media::nv12_size(renderer.width(), renderer.height()));
    std::vector<std::vector<uint8_t>> out;
    for (const int64_t f : frames) {
      ATM_CHECK(renderer.render(f, nv12.data()));
      std::vector<uint8_t> &bgrx = out.emplace_back(size_t(renderer.width()) * size_t(renderer.height()) * 4);
      media::nv12_to_bgrx(nv12.data(), renderer.width(), renderer.height(), bgrx.data());
    }
    *warning = renderer.take_warning();
    return out;
  }

  // Shared set-up: the compiled sequence and an emptied output folder. Earlier pictures are removed, so the folder
  // never grows; a client reads the files before its next see.* call.
  Result<std::pair<render::Composition, fs::path>> see_setup(Project &pr, const json &params) {
    ATM_TRY(render::Composition comp, render::compile(pr.doc.root(), params.value("sequence", std::string()), to_utf8(pr.dir)));
    if (comp.frames <= 0)
      return fail(ErrorCode::InvalidArgument, "R_EMPTY", "The sequence is empty, so there is nothing to see.", {},
                  "Add clips with project.patch first.");
    const fs::path dir = pr.dir / ".attome" / "see";
    ATM_CHECK(storage::make_dirs(dir));
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(dir, ec))
      if (entry.path().extension() == ".jpg")
        fs::remove(entry.path(), ec);
    return std::pair{std::move(comp), dir};
  }

  static json time_of(int64_t frame, const render::Composition &comp) {
    const Rational rate = *Rational::make(comp.rate_num, comp.rate_den);
    const auto t = from_frames(frame, rate);
    json out = t ? to_json(format_time(*t, rate)) : json::object();
    out["frame"] = frame;
    return out;
  }

  Result<json> see_frames(const json &params) {
    ATM_TRY(Project *pr, project(params));
    const auto times = params.find("times");
    if (times == params.end() || !times->is_array() || times->empty() || times->size() > 16)
      return bad_param("times", "is required: 1 to 16 times such as \"2.5s\", \"75@30\" or \"00:00:02:15\"");
    const int height = params.value("height", 540);
    if (height < 64 || height > 2160)
      return bad_param("height", "must be between 64 and 2160 pixels");
    ATM_TRY(auto setup, see_setup(*pr, params));
    auto &[comp, dir] = setup;
    const Rational rate = *Rational::make(comp.rate_num, comp.rate_den);
    std::vector<int64_t> frames;
    for (const json &value : *times) {
      ATM_TRY(RationalTime t, parse_time(value, {.rate = rate}));
      ATM_TRY(int64_t f, to_frames(t, rate, Round::floor));
      frames.push_back(std::clamp<int64_t>(f, 0, comp.frames - 1)); // a time past the end shows the last frame
    }
    const int width = int(int64_t(comp.width) * height / std::max(1, comp.height));
    std::string warning;
    json images = json::array();
    {
      ATM_TRY(auto stills, render_stills(comp, frames, width, height, &warning));
      for (size_t i = 0; i < frames.size(); ++i) {
        const fs::path file = dir / ("r" + std::to_string(pr->revision) + "-f" + std::to_string(frames[i]) + ".jpg");
        ATM_CHECK(media::write_jpeg(to_utf8(file), stills[i].data(), width & ~1, height & ~1));
        json image = time_of(frames[i], comp);
        image["path"] = to_utf8(file);
        images.push_back(std::move(image));
      }
    }
    json out = {{"images", std::move(images)}, {"width", width & ~1}, {"height", height & ~1},
                {"duration", time_of(comp.frames, comp)}, {"revision", pr->revision}};
    if (!warning.empty())
      out["warning"] = warning;
    return out;
  }

  // One JPEG with `count` evenly spaced frames in a grid, each with its timecode underneath.
  Result<json> see_contact_sheet(const json &params) {
    ATM_TRY(Project *pr, project(params));
    const int count = params.value("count", 12), columns = std::clamp(params.value("columns", 4), 1, 8);
    const int tile_h = params.value("tile_height", 180);
    if (count < 1 || count > 48)
      return bad_param("count", "must be between 1 and 48");
    if (tile_h < 64 || tile_h > 540)
      return bad_param("tile_height", "must be between 64 and 540 pixels");
    ATM_TRY(auto setup, see_setup(*pr, params));
    auto &[comp, dir] = setup;
    std::vector<int64_t> frames; // the middle of each of `count` equal parts
    for (int i = 0; i < count; ++i)
      frames.push_back(std::min(comp.frames - 1, (2 * i + 1) * comp.frames / (2 * count)));
    frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
    const int tile_w = int(int64_t(comp.width) * tile_h / std::max(1, comp.height)) & ~1;
    const int th = tile_h & ~1, label_h = std::max(16, th / 7), gap = 4;
    const int n = int(frames.size()), cols = std::min(columns, n), rows = (n + cols - 1) / cols;
    const int sheet_w = cols * tile_w + (cols + 1) * gap, sheet_h = rows * (th + label_h) + (rows + 1) * gap;
    std::vector<uint8_t> sheet(size_t(sheet_w) * size_t(sheet_h) * 4);
    for (size_t i = 0; i < sheet.size(); i += 4) { // dark grey background
      sheet[i] = sheet[i + 1] = sheet[i + 2] = 24;
      sheet[i + 3] = 255;
    }
    std::string warning;
    ATM_TRY(auto stills, render_stills(comp, frames, tile_w, th, &warning));
    json tiles = json::array();
    for (int i = 0; i < n; ++i) {
      const int x0 = gap + (i % cols) * (tile_w + gap), y0 = gap + (i / cols) * (th + label_h + gap);
      for (int y = 0; y < th; ++y)
        std::memcpy(&sheet[(size_t(y0 + y) * size_t(sheet_w) + size_t(x0)) * 4],
                    &stills[size_t(i)][size_t(y) * size_t(tile_w) * 4], size_t(tile_w) * 4);
      json tile = time_of(frames[size_t(i)], comp);
      if (auto label = media::render_text(tile.value("timecode", ""), float(label_h) * 0.7f, false, tile_w)) {
        const int lx = x0 + std::max(0, (tile_w - label->width) / 2), ly = y0 + th + (label_h - label->height) / 2;
        for (int y = 0; y < label->height; ++y)
          for (int x = 0; x < label->width; ++x) {
            const int sx = lx + x, sy = ly + y;
            if (sx < 0 || sy < 0 || sx >= sheet_w || sy >= sheet_h)
              continue;
            uint8_t *px = &sheet[(size_t(sy) * size_t(sheet_w) + size_t(sx)) * 4];
            const int a = label->alpha[size_t(y) * size_t(label->width) + size_t(x)];
            for (int c = 0; c < 3; ++c)
              px[c] = uint8_t(px[c] + (230 - px[c]) * a / 255);
          }
      }
      tiles.push_back(std::move(tile));
    }
    const fs::path file = dir / ("r" + std::to_string(pr->revision) + "-sheet.jpg");
    ATM_CHECK(media::write_jpeg(to_utf8(file), sheet.data(), sheet_w, sheet_h));
    json out = {{"images", json::array({{{"path", to_utf8(file)}}})},
                {"tiles", std::move(tiles)},
                {"columns", cols},
                {"width", sheet_w},
                {"height", sheet_h},
                {"duration", time_of(comp.frames, comp)},
                {"revision", pr->revision}};
    if (!warning.empty())
      out["warning"] = warning;
    return out;
  }

  // ---- media.import and timeline.edit (F1 §5.8, §5.10) ---------------------------------------------------------

  // Files become assets of the project (root "assets"), with what media.probe found. A path already imported is reused.
  Result<json> media_import(const json &params) {
    ATM_TRY(Project *pr, project(params));
    const auto paths = params.find("paths");
    if (paths == params.end() || !paths->is_array() || paths->empty())
      return bad_param("paths", "is required: a list of absolute file paths");
    const json &root = pr->doc.root();
    json ops = json::array(), assets = json::array();
    std::vector<std::pair<std::string, size_t>> pending; // placeholder -> index in `assets`
    for (size_t i = 0; i < paths->size(); ++i) {
      if (!(*paths)[i].is_string())
        return bad_param("paths", "must hold strings");
      const std::string path = (*paths)[i].get<std::string>();
      ATM_TRY(json info, media_probe({{"path", path}}));
      std::string existing;
      if (root.contains("assets"))
        for (auto it = root["assets"].begin(); it != root["assets"].end(); ++it)
          if (it->value("path", "") == path)
            existing = it.key();
      const std::u8string file = to_path(path).filename().u8string();
      json asset = {{"name", std::string(file.begin(), file.end())}};
      for (auto it = info.begin(); it != info.end(); ++it)
        asset[it.key()] = it.value();
      if (!existing.empty()) {
        asset["id"] = existing;
        assets.push_back(std::move(asset));
        continue;
      }
      const std::string ph = "$new:asset" + std::to_string(i);
      ops.push_back({{"op", "add"}, {"path", pr->doc.id() + "/assets/" + ph}, {"value", asset}});
      pending.emplace_back(ph, assets.size());
      assets.push_back(std::move(asset));
    }
    uint64_t revision = pr->revision;
    if (!ops.empty()) {
      json p = {{"project", params["project"]},
                {"patch", {{"ops", std::move(ops)}, {"label", "Import " + std::to_string(pending.size()) + " file(s)"}}}};
      if (params.contains("task_id"))
        p["task_id"] = params["task_id"];
      ATM_TRY(json res, project_patch(p));
      for (const auto &[ph, at] : pending)
        assets[at]["id"] = res["id_map"].value(ph, "");
      revision = res.value("revision", revision);
    }
    return json{{"assets", std::move(assets)}, {"revision", revision}};
  }

  // Replaces every string equal to a placeholder of an earlier op with the ID it became.
  static void resolve_placeholders(json &v, const json &id_map) {
    if (v.is_string()) {
      if (const auto it = id_map.find(v.get_ref<const std::string &>()); it != id_map.end())
        v = *it;
    } else if (v.is_structured()) {
      for (json &e : v)
        resolve_placeholders(e, id_map);
    }
  }

  // Each op becomes patch ops against a scratch copy holding the earlier ops, so later ops see what earlier ones made;
  // the whole call is then applied to the project as one Patch (validated, journaled, one undo step per task).
  Result<json> timeline_edit(const json &params) {
    ATM_TRY(Project *pr, project(params));
    const auto ops = params.find("ops");
    if (ops == params.end() || !ops->is_array() || ops->empty())
      return bad_param("ops", "is required: a list of timeline ops (guide.get topic \"timeline\")");
    const json &root = pr->doc.root();
    timeline::Context ctx;
    ctx.sequence = params.value("sequence", std::string());
    if (ctx.sequence.empty() && root.contains("sequence_order") && !root["sequence_order"].empty())
      ctx.sequence = root["sequence_order"][0].get<std::string>();
    if (!root.contains("sequences") || !root["sequences"].contains(ctx.sequence))
      return fail(ErrorCode::NotFound, "R_NO_SEQUENCE", "The project has no sequence \"" + ctx.sequence + "\".");
    ATM_TRY(Rational rate, Rational::parse(root["sequences"][ctx.sequence].value("rate", std::string("30"))));
    ctx.rate = rate;
    ctx.probe = [this](const std::string &path) { return media_probe({{"path", path}}); };

    ATM_TRY(doc::Document scratch, doc::Document::from_json(root));
    json all = json::array(), id_map = json::object(), notes = json::array();
    for (size_t i = 0; i < ops->size(); ++i) {
      json op = (*ops)[i];
      resolve_placeholders(op, id_map);
      if (op.is_object() && op.contains("id")) // its own name stays a placeholder
        op["id"] = (*ops)[i]["id"];
      ATM_TRY(timeline::Built built, timeline::build(scratch, op, i, ctx));
      auto applied = patch::apply(scratch, built.ops, {.keep = true, .validate = false});
      if (!applied) {
        Error e = std::move(applied.error());
        e.message = "ops[" + std::to_string(i) + "] (" + op.value("op", std::string("?")) + "): " + e.message;
        e.details = {{"op_index", i}};
        return tl::unexpected(std::move(e));
      }
      for (auto it = built.names.begin(); it != built.names.end(); ++it)
        if (const auto made = applied->id_map.find(it.key()); made != applied->id_map.end())
          id_map[it.key()] = *made;
      for (json &o : applied->ops)
        all.push_back(std::move(o));
      for (json &n : built.notes)
        notes.push_back(std::move(n));
    }
    json p = {{"project", params["project"]},
              {"patch", {{"ops", std::move(all)}, {"label", params.value("label", "Timeline: " + std::to_string(ops->size()) + " op(s)")}}},
              {"dry_run", params.value("dry_run", false)}};
    if (params.contains("task_id"))
      p["task_id"] = params["task_id"];
    ATM_TRY(json res, project_patch(p));
    res["id_map"] = std::move(id_map);
    res["notes"] = std::move(notes);
    res["duration"] = timeline::sequence_duration(scratch, ctx);
    if (!res.value("applied", false) && res.contains("inverse")) // a dry run: the inverse is noise for an agent
      res.erase("inverse");
    return res;
  }

  // ---- gen.* (Clip Workflows: what can run here) -----------------------------------------------------------------

  // Every file of the model is on this computer: in the models folder or in a folder the user pointed at. A model
  // that is not in the catalog has no files to look for.
  bool model_installed(std::string_view id) const {
    const models::CatalogEntry *entry = models::find_entry(models::builtin_catalog(), id);
    if (!entry)
      return false;
    const fs::path dir = models_dir();
    for (const models::CatalogFile &f : entry->files)
      if (models::file_status(f, dir, model_folders).state != models::FileState::installed)
        return false;
    return true;
  }

  static const json &library_of(const Project &pr) {
    static const json none = json::object();
    const auto it = pr.doc.root().find("workflows");
    return it != pr.doc.root().end() && it->is_object() ? *it : none;
  }

  // A clip's Instance: its own copy of a Clip Workflow.
  static const json &instance_of(const Project &pr, const std::string &clip_id) {
    static const json none = json::object();
    const doc::NodeRef *ref = pr.doc.find(clip_id);
    if (!ref || !ref->node->is_object())
      return none;
    const auto media = ref->node->find("media_ref");
    if (media == ref->node->end() || !media->is_object())
      return none;
    const auto instance = media->find("workflow");
    return instance != media->end() && instance->is_object() ? *instance : none;
  }

  // What keeps a clip's workflow from running on this machine (a model not chosen, not known, or not installed), as
  // JSON, with the model's ID and, for one that can be downloaded, its title and what is still missing.
  json model_warnings(const Project &pr, const std::string &clip_id) const {
    std::vector<gen::Problem> found;
    gen::check_models(library_of(pr), instance_of(pr, clip_id), clip_id, [&](std::string_view id) { return model_installed(id); }, found);
    json out = json::array();
    for (gen::Problem &p : found) {
      json w = {{"rule", p.rule}, {"path", p.path}, {"target", p.target}, {"message", p.message}, {"hint", p.hint}};
      const std::string node = p.path.substr(0, p.path.find('/'));
      if (const doc::NodeRef *ref = pr.doc.find(node); ref && ref->node->contains("model") && (*ref->node)["model"].is_string()) {
        const std::string model = (*ref->node)["model"].get<std::string>();
        w["model"] = model;
        if (const models::CatalogEntry *entry = models::find_entry(models::builtin_catalog(), model)) {
          int64_t on_disk = 0;
          const fs::path dir = models_dir();
          for (const models::CatalogFile &f : entry->files)
            on_disk += models::file_status(f, dir, model_folders).bytes;
          w["title"] = entry->title;
          w["size"] = entry->size();
          w["bytes_missing"] = entry->size() - on_disk;
          w["can_download"] = true;
        }
      }
      out.push_back(std::move(w));
    }
    return out;
  }

  // What a model is on this machine, for cache keys: the engine that runs it and the hashes of its files.
  gen::KeyContext key_context(const Project &pr) const {
    gen::KeyContext context;
    context.variables = pr.doc.root().value("variables", json::object()); // what Variable nodes read
    context.model_identity = [this](std::string_view model) {
      std::string identity;
      for (const auto &p : providers)
        identity += p->fingerprint(model);
      if (const models::CatalogEntry *entry = models::find_entry(models::builtin_catalog(), model))
        for (const models::CatalogFile &f : entry->files)
          identity += ":" + f.sha256;
      return identity;
    };
    return context;
  }

  static fs::path gen_dir(const Project &pr) { return pr.dir / ".attome" / "gen"; }

  // The generative clips of a project, in timeline order, with what their Input nodes read: the canvas and rate of their
  // Sequence, their Duration and start, and the clips before and after them on their track.
  static std::vector<gen::ClipIn> gen_clips(const Project &pr) {
    std::vector<gen::ClipIn> out;
    const json &root = pr.doc.root();
    const auto seqs = root.find("sequences");
    if (seqs == root.end() || !seqs->is_object())
      return out;
    for (auto s = seqs->begin(); s != seqs->end(); ++s) {
      const auto tracks = s->find("tracks");
      if (tracks == s->end() || !tracks->is_object())
        continue;
      const json canvas = s->value("canvas", json::object());
      const auto rate = Rational::parse(s->value("rate", std::string("30")));
      for (auto t = tracks->begin(); t != tracks->end(); ++t) {
        const auto clips = t->find("clips");
        if (clips == t->end() || !clips->is_object())
          continue;
        struct Placed {
          std::string id;
          double start;
        };
        std::vector<Placed> by_time; // every clip of the track, left to right: "previous" and "next" are among all of them
        for (auto c = clips->begin(); c != clips->end(); ++c) {
          double start = 0.0;
          if (const auto timing = c->find("timing"); timing != c->end() && timing->is_object())
            if (const auto in = Rational::parse(timing->value("record_in", std::string("0"))))
              start = in->to_seconds_lossy();
          by_time.push_back({c.key(), start});
        }
        std::stable_sort(by_time.begin(), by_time.end(), [](const Placed &a, const Placed &b) { return a.start < b.start; });
        for (size_t i = 0; i < by_time.size(); ++i) {
          const auto c = clips->find(by_time[i].id);
          const auto ref = c->find("media_ref");
          if (ref == c->end() || !ref->is_object() || ref->value("type", std::string()) != "workflow")
            continue;
          gen::ClipIn in{c.key(), c->value("name", c.key()), &*ref, int64_t(by_time[i].start * 1000.0)};
          const json timing = c->value("timing", json::object());
          const auto duration = Rational::parse(timing.value("duration", std::string("0")));
          in.facts.known = true;
          in.facts.start = by_time[i].start;
          in.facts.duration = duration ? duration->to_seconds_lossy() : 0.0;
          in.facts.width = canvas.value("width", 1920);
          in.facts.height = canvas.value("height", 1080);
          in.facts.frame_rate = rate ? rate->to_seconds_lossy() : 30.0;
          in.previous = i > 0 ? by_time[i - 1].id : std::string();
          in.next = i + 1 < by_time.size() ? by_time[i + 1].id : std::string();
          out.push_back(std::move(in));
        }
      }
    }
    return out;
  }

  std::vector<gen::ClipPlan> gen_plan(const Project &pr, gen::PlanOptions options) const {
    options.present = [&pr](const json &take) { // every file the Take recorded is still there
      const auto outputs = take.find("outputs");
      if (outputs == take.end() || !outputs->is_object())
        return false;
      for (const json &o : *outputs)
        if (const fs::path file = to_path(o.value("path", std::string())); !storage::exists(file.is_absolute() ? file : pr.dir / file))
          return false;
      return !outputs->empty();
    };
    return gen::plan(library_of(pr), gen_clips(pr), key_context(pr), options);
  }

  static json plan_json(const gen::ClipPlan &p) {
    json c = {{"clip", p.id}, {"name", p.name}, {"source", p.source}, {"state", gen::clip_state_name(p.state)}, {"run", p.run},
              {"depends_on", p.depends}};
    if (!p.reason.empty())
      c["reason"] = p.reason;
    if (!p.skip.empty())
      c["skip"] = p.skip;
    if (p.out_of_step)
      c["out_of_step"] = true;
    return c;
  }

  // A model is chosen, known and installed, but nothing here runs it.
  void engine_warnings(const Project &pr, const json &workflow, const std::string &owner, json &out, std::set<std::string> &seen) const {
    const auto nodes = workflow.is_object() ? workflow.find("nodes") : workflow.end();
    if (!workflow.is_object() || nodes == workflow.end() || !nodes->is_object())
      return;
    for (auto it = nodes->begin(); it != nodes->end(); ++it) {
      const std::string kind = it->value("kind", std::string());
      if (gen::is_workflow_kind(kind)) { // a library workflow used as a node
        const std::string inner = it->value("workflow", std::string());
        if (seen.insert(inner).second)
          if (const auto wf = library_of(pr).find(inner); wf != library_of(pr).end())
            engine_warnings(pr, *wf, inner, out, seen);
        continue;
      }
      const gen::KindDef *def = gen::find_kind(kind);
      const std::string model = it->contains("model") && (*it)["model"].is_string() ? (*it)["model"].get<std::string>() : std::string();
      const gen::ModelDecl *decl = def ? gen::find_model(model) : nullptr;
      if (!decl || (decl->needs_files && !model_installed(model)) || provider_for(providers, model, def->id))
        continue;
      out.push_back({{"rule", "G_ENGINE_MISSING"}, {"path", it.key() + "/model"}, {"target", owner}, {"model", model},
                     {"message", "Nothing on this computer runs the model " + model + " yet."},
                     {"hint", "Set the address of your ComfyUI in the Models panel, or wait for Attome's own engine for this model."}});
    }
  }

  // What keeps a clip from running: its models (not chosen, not known, not installed, not run by anything here), and what
  // its workflow still needs (an input with nothing behind it, no Primary Output, a value the clip did not give).
  json ready_problems(const Project &pr, const std::string &clip_id) const {
    json problems = model_warnings(pr, clip_id);
    std::set<std::string> seen;
    engine_warnings(pr, instance_of(pr, clip_id), clip_id, problems, seen);
    const doc::NodeRef *ref = pr.doc.find(clip_id);
    if (!ref)
      return problems;
    const gen::ClipLookup lookup = [&](std::string_view id) -> const json * {
      const doc::NodeRef *other = pr.doc.find(id);
      return other ? other->node : nullptr;
    };
    std::vector<gen::Problem> found;
    gen::check_clip(library_of(pr), pr.doc.root().value("variables", json::object()), clip_id, *ref->node, lookup, found);
    for (gen::Problem &p : found)
      if (gen::is_readiness_rule(p.rule))
        problems.push_back({{"rule", p.rule}, {"path", p.path}, {"target", p.target}, {"message", p.message}, {"hint", p.hint}});
    return problems;
  }

  // The node kinds a workflow is built from, with their ports, and every model with what it declares: the kinds it runs,
  // its settings with their ranges and defaults, the optional inputs it takes. What a workflow editor needs to offer
  // only what the validator will accept.
  Result<json> gen_nodes(const json &) {
    const auto ports = [](std::span<const gen::PortDef> defs) {
      json out = json::array();
      for (const gen::PortDef &d : defs)
        out.push_back({{"name", d.name}, {"type", gen::port_type_name(d.type)}, {"required", d.required}, {"list", d.list}});
      return out;
    };
    json kinds = json::array();
    for (const gen::KindDef &k : gen::kind_defs())
      kinds.push_back({{"id", k.id}, {"kind", gen::kind_name(k)}, {"title", k.title}, {"runs_model", k.runs_model}, {"input", k.is_input},
                       {"inputs", ports(k.inputs)}, {"outputs", ports(k.outputs)}});
    json models = json::array();
    for (const std::string &id : gen::model_ids()) {
      const gen::ModelDecl *decl = gen::find_model(id);
      if (!decl)
        continue;
      const models::CatalogEntry *entry = models::find_entry(models::builtin_catalog(), id);
      json settings = json::array();
      for (const gen::SettingDecl &s : decl->settings) {
        static const char *const names[] = {"integer", "number", "boolean", "choice", "text"};
        json one = {{"name", s.name}, {"type", names[int(s.type)]}, {"default", s.def}};
        if (s.type == gen::SettingDecl::Type::integer || s.type == gen::SettingDecl::Type::number) {
          one["min"] = s.lo;
          one["max"] = s.hi;
        }
        if (s.type == gen::SettingDecl::Type::choice)
          one["options"] = s.options;
        settings.push_back(std::move(one));
      }
      const bool installed = !decl->needs_files || model_installed(id);
      json engines = json::object(); // per kind: does something here run it
      for (const std::string &kind : decl->kinds)
        engines[kind] = provider_for(providers, id, kind) != nullptr;
      models.push_back({{"id", id}, {"title", entry ? entry->title : id}, {"kinds", decl->kinds}, {"accepts", decl->accepts},
                        {"settings", std::move(settings)}, {"installed", installed}, {"engines", std::move(engines)},
                        {"seconds", {{"min", decl->seconds_min}, {"max", decl->seconds_max}}}});
    }
    return json{{"kinds", std::move(kinds)}, {"models", std::move(models)}};
  }

  Result<json> gen_status(const json &params) {
    ATM_TRY(Project *pr, project(params));
    json clips = json::array();
    const std::vector<gen::ClipIn> all = gen_clips(*pr);
    for (const gen::ClipPlan &p : gen_plan(*pr, {})) {
      json c = plan_json(p);
      c.erase("run");
      json problems = ready_problems(*pr, p.id);
      c["ready"] = problems.empty();
      c["problems"] = std::move(problems);
      for (const gen::ClipIn &in : all)
        if (in.id == p.id) {
          c["takes"] = in.ref->contains("takes") && (*in.ref)["takes"].is_object() ? (*in.ref)["takes"].size() : size_t(0);
          c["selected"] = in.ref->contains("selected") ? (*in.ref)["selected"] : json(nullptr);
        }
      clips.push_back(std::move(c));
    }
    return json{{"clips", std::move(clips)}, {"revision", pr->revision}};
  }

  Result<json> gen_run(const json &params) {
    ATM_TRY(Project *pr, project(params));
    gen::PlanOptions options;
    const std::string scope = params.value("scope", std::string(params.contains("clips") ? "selected" : "dirty"));
    if (scope == "dirty")
      options.scope = gen::Scope::dirty;
    else if (scope == "all")
      options.scope = gen::Scope::all;
    else if (scope == "selected")
      options.scope = gen::Scope::selected;
    else if (scope == "selected_and_after")
      options.scope = gen::Scope::selected_and_after;
    else
      return bad_param("scope", "must be dirty, all, selected or selected_and_after");
    if (const auto clips = params.find("clips"); clips != params.end()) {
      if (!clips->is_array())
        return bad_param("clips", "must be a list of clip IDs");
      for (const json &c : *clips)
        if (c.is_string())
          options.clips.push_back(c.get<std::string>());
    }
    const bool named = options.scope == gen::Scope::selected || options.scope == gen::Scope::selected_and_after;
    if (named && options.clips.empty())
      return bad_param("clips", "is required for the scopes selected and selected_and_after");
    const bool dry_run = params.value("dry_run", false);
    for (const auto &[id, job] : jobs)
      if (job->kind == "gen.run" && job->output == to_utf8(pr->dir) && job->state.load() == Job::running && !dry_run)
        return fail(ErrorCode::InvalidArgument, "G_BUSY", "A generation is already running for this project.", {},
                    "Follow it with jobs.get " + id + ", or stop it with jobs.cancel.");

    // A new Take of the named clips: the same inputs with the next seed.
    if (params.value("new_take", false) && !dry_run) {
      if (!named)
        return bad_param("new_take", "needs \"clips\": the clips to make another Take of");
      json ops = json::array();
      for (const gen::ClipIn &c : gen_clips(*pr)) {
        if (std::find(options.clips.begin(), options.clips.end(), c.id) == options.clips.end())
          continue;
        // Only a workflow that has a seed input can vary by it. The next seed is above the clip's own and above the seed of every
        // Take it has made, so a new Take never repeats an earlier one.
        if (!c.ref->value("workflow", json::object()).value("exposed", json::object()).value("inputs", json::object()).contains("seed"))
          continue;
        const json inputs = c.ref->value("inputs", json::object());
        int64_t next = inputs.contains("seed") && inputs["seed"].is_number_integer() ? inputs["seed"].get<int64_t>() : int64_t(0);
        const json takes = c.ref->value("takes", json::object());
        for (auto t = takes.begin(); t != takes.end(); ++t)
          if (const json then = t->value("inputs", json::object()); then.contains("seed") && then["seed"].is_number_integer())
            next = std::max(next, then["seed"].get<int64_t>());
        ops.push_back({{"op", inputs.contains("seed") ? "replace" : "add"}, {"path", c.id + "/media_ref/inputs/seed"}, {"value", next + 1}});
      }
      if (!ops.empty())
        ATM_CHECK(project_patch({{"project", to_utf8(pr->dir)}, {"patch", {{"ops", std::move(ops)}, {"label", "New take"}}}}).map([](const json &) {}));
    }

    const std::vector<gen::ClipPlan> plans = gen_plan(*pr, options);
    GenRun run;
    json listed = json::array();
    for (const gen::ClipPlan &p : plans) {
      listed.push_back(plan_json(p));
      if (!p.run)
        continue;
      if (const json problems = ready_problems(*pr, p.id); !problems.empty()) { // refused before anything runs
        Error e;
        e.code = ErrorCode::InvalidArgument;
        e.rule = "G_NOT_READY";
        e.path = p.id;
        e.message = p.name + " cannot be generated: " + problems[0].value("message", std::string());
        e.hint = problems[0].value("hint", std::string());
        e.errors = problems;
        return tl::unexpected(std::move(e));
      }
      GenClip clip;
      clip.id = p.id;
      clip.name = p.name;
      clip.instance = instance_of(*pr, p.id);
      clip.key = p.key;
      clip.inputs = p.inputs;
      clip.depends = p.depends;
      clip.references = p.references;
      for (const gen::ClipIn &in : gen_clips(*pr))
        if (in.id == p.id)
          clip.facts = in.facts;
      if (const doc::NodeRef *ref = pr->doc.find(p.id))
        clip.written = ref->node->value("media_ref", json::object()).value("inputs", json::object());
      run.clips.push_back(std::move(clip));
    }
    run.library = pr->doc.root().value("workflows", json::object());
    run.dir = gen_dir(*pr);
    run.project = pr->dir;
    run.providers = providers;
    run.context = key_context(*pr);
    const StepCount count = count_steps(run);
    json out = {{"plan", std::move(listed)}, {"clips", run.clips.size()}, {"steps", count.total}, {"steps_cached", count.cached}};
    if (dry_run || run.clips.empty()) {
      out["job_id"] = nullptr;
      return out;
    }
    ATM_CHECK(storage::make_dirs(run.dir));
    auto job = std::make_shared<Job>();
    job->id = new_id("job");
    job->kind = "gen.run";
    job->output = to_utf8(pr->dir);
    job->units_total.store(count.total);
    jobs[job->id] = job;
    job->thread = std::thread(run_gen, job, std::move(run), finished, to_utf8(pr->dir));
    out["job_id"] = job->id;
    return out;
  }

  // Puts the Takes that job threads finished on their clips, each as one undoable edit that also selects it.
  void apply_finished() {
    std::vector<Finished> items;
    {
      std::lock_guard lock(finished->mutex);
      items.swap(finished->items);
    }
    for (Finished &f : items) {
      const auto pr = project({{"project", f.project}});
      const doc::NodeRef *ref = pr ? (*pr)->doc.find(f.clip) : nullptr;
      if (!ref)
        continue; // the clip was deleted while it was being generated
      const json media = ref->node->value("media_ref", json::object());
      const bool had = media.contains("selected");
      // The same result again (everything came from the cache): the Take that already holds it is selected, not doubled.
      std::string same;
      const json takes = media.value("takes", json::object());
      for (auto t = takes.begin(); t != takes.end(); ++t)
        if (t->value("key", std::string()) == f.take.value("key", std::string()) && t->value("outputs", json()) == f.take.value("outputs", json()))
          same = t.key();
      if (!same.empty() && media.value("selected", json()) == json(same))
        continue;
      json ops = json::array();
      if (same.empty())
        ops.push_back({{"op", "add"}, {"path", f.clip + "/media_ref/takes/$new:take"}, {"value", std::move(f.take)}});
      ops.push_back({{"op", had ? "replace" : "add"}, {"path", f.clip + "/media_ref/selected"}, {"value", same.empty() ? std::string("$new:take") : same}});
      (void)project_patch({{"project", f.project}, {"patch", {{"ops", std::move(ops)}, {"label", "Generate " + f.name}}}});
    }
  }

  // The user's settings: one small JSON file, read when asked for. An empty path = none (tests).
  fs::path settings_path() const {
    if (!cfg.user_settings)
      return {};
    if (const char *path = std::getenv("ATTOME_SETTINGS"); path && *path)
      return to_path(path);
#ifdef _WIN32
    if (const char *local = std::getenv("LOCALAPPDATA"); local && *local)
      return to_path(local) / "Attome" / "settings.json";
#else
    if (const char *home = std::getenv("HOME"); home && *home)
      return to_path(home) / ".config" / "attome" / "settings.json";
#endif
    return {};
  }

  json settings() const {
    const fs::path path = settings_path();
    if (path.empty())
      return json::object();
    const auto text = storage::read_file(path);
    const json parsed = text ? json::parse(*text, nullptr, false) : json();
    return parsed.is_object() ? parsed : json::object();
  }

  // The ComfyUI engine at this address replaces the one there was; an empty address removes it.
  void set_comfyui(const std::string &address) {
    std::erase_if(providers, [](const std::shared_ptr<gen::Provider> &p) { return p->name() == "comfyui"; });
    comfyui_address = address;
    if (!address.empty())
      providers.push_back(std::make_shared<ComfyProvider>(
          address, cfg.transport ? cfg.transport : std::shared_ptr<net::Transport>(net::system_transport())));
  }

  Result<json> gen_engines(const json &) {
    json list = json::array();
    for (const auto &p : providers)
      list.push_back(p->status());
    return json{{"engines", std::move(list)}, {"comfyui", comfyui_address}};
  }

  Result<json> gen_set_comfyui(const json &params) {
    ATM_TRY(const std::string *address, string_param(params, "address"));
    std::string trimmed = *address;
    while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '/'))
      trimmed.pop_back();
    while (!trimmed.empty() && trimmed.front() == ' ')
      trimmed.erase(trimmed.begin());
    if (!trimmed.empty() && trimmed.find("://") == std::string::npos)
      trimmed = "http://" + trimmed;
    set_comfyui(trimmed);
    if (const fs::path path = settings_path(); !path.empty()) {
      json all = settings();
      all["comfyui"] = trimmed;
      ATM_CHECK(storage::make_dirs(path.parent_path()));
      ATM_CHECK(storage::atomic_write(path, all.dump(2) + "\n"));
    }
    json out = {{"comfyui", trimmed}};
    if (!trimmed.empty())
      out["status"] = providers.back()->status();
    return out;
  }

  // The models a generative clip can be made with, each with whether it can run here now.
  Result<json> gen_models(const json &) {
    json list = json::array();
    for (const std::string &id : gen::model_ids()) {
      const gen::ModelDecl *decl = gen::find_model(id);
      if (!decl || !decl->does("generate_video"))
        continue;
      const models::CatalogEntry *entry = models::find_entry(models::builtin_catalog(), id);
      const bool installed = !decl->needs_files || model_installed(id);
      const bool engine = provider_for(providers, id, "generate_video") != nullptr;
      // The kind of clip the model makes (the Generate panel groups by it) and, within it, the model family
      // (SD 1.5, SDXL, ...). Only video models exist so far; image models will say theirs.
      list.push_back({{"id", id}, {"title", entry ? entry->title : id}, {"clip_type", "video"}, {"family", ""},
                      {"installed", installed}, {"engine", engine},
                      {"ready", installed && engine}, {"accepts", decl->accepts},
                      {"seconds", {{"min", decl->seconds_min}, {"max", decl->seconds_max}}}});
    }
    return json{{"models", std::move(list)}};
  }

  // Adds a generative clip made from the built-in Shot of the model: the clip gets its own copy of that Clip Workflow (its
  // Instance), at the end of the picture track unless a place is given.
  Result<json> gen_create_clip(const json &params) {
    ATM_TRY(Project *pr, project(params));
    ATM_TRY(const std::string *prompt, string_param(params, "prompt"));
    const json &root = pr->doc.root();
    // Made from a built-in Clip Workflow (a model: its Shot), or from one of the project's library (a "cwf_…" ID).
    const std::string library_id = params.value("workflow", std::string());
    json instance;
    const gen::ModelDecl *decl = nullptr;
    if (!library_id.empty()) {
      if (params.contains("model"))
        return bad_param("model", "is not used together with \"workflow\": the workflow has its own models");
      const json &library = library_of(*pr);
      const auto wf = library.find(library_id);
      if (wf == library.end() || !wf->is_object())
        return fail(ErrorCode::NotFound, "G_WORKFLOW", "The project's library has no workflow \"" + library_id + "\".", {},
                    "List the library with project.get on the project's ID: its \"workflows\".");
      instance = gen::fresh_copy(*wf, library_id);
      decl = gen::main_model(*wf);
    } else {
      ATM_TRY(const std::string *model, string_param(params, "model"));
      decl = gen::find_model(*model);
      if (!decl || !decl->does("generate_video"))
        return fail(ErrorCode::NotFound, "G_MODEL", "\"" + *model + "\" is not a model that generates video here.", {},
                    "List the models with gen.models.");
      instance = gen::instantiate("shot:" + *model);
    }
    const json &face = instance.value("exposed", json::object()).value("inputs", json::object());
    const std::string project_ref = to_utf8(pr->dir);
    std::string seq = params.value("sequence", std::string());
    if (seq.empty() && root.contains("sequence_order") && !root["sequence_order"].empty())
      seq = root["sequence_order"][0].get<std::string>();
    const doc::NodeRef *seq_ref = pr->doc.find(seq);
    if (!seq_ref)
      return fail(ErrorCode::UnknownId, "P_UNKNOWN_ID", "The project has no sequence \"" + seq + "\".");
    const json &sequence = *seq_ref->node;

    // The size the clip is made at: the canvas's shape at about 0.9 megapixels, on the model's grid (what the Shot's Project
    // node and the run give the model). It is kept on the clip so the editor can scale the picture to cover the canvas.
    const json canvas = sequence.value("canvas", json::object());
    const double cw = canvas.value("width", 1920), ch = canvas.value("height", 1080);
    const auto scaled = gen::scaled_size(int64_t(cw), int64_t(ch), gen::kGenerationPixels);
    const auto [made_w, made_h] = decl ? gen::fit_size(*decl, scaled.first, scaled.second) : scaled;
    const int width = int(made_w), height = int(made_h);
    double seconds = std::max(params.value("seconds", 5.0), 0.1);
    if (decl) {
      seconds = std::max(seconds, decl->seconds_min);
      if (decl->seconds_max > 0.0)
        seconds = std::min(seconds, decl->seconds_max);
    }
    static std::atomic<uint32_t> counter{uint32_t(std::chrono::steady_clock::now().time_since_epoch().count())};
    const int64_t seed = params.value("seed", int64_t((counter.fetch_add(2654435761u) >> 8) % 1000000));

    // The track: the one named, else the lowest picture track that is not for titles or effects, else a new one.
    json ops = json::array();
    std::string track = params.value("track", std::string());
    const json &tracks = sequence.contains("tracks") ? sequence["tracks"] : json::object();
    if (track.empty())
      for (const json &id : sequence.value("track_order", json::array())) {
        const auto t = tracks.find(id.get<std::string>());
        if (t != tracks.end() && t->value("kind", std::string("video")) != "audio" && t->value("name", std::string()) != "Titles" &&
            t->value("name", std::string()) != "Effects") {
          track = id.get<std::string>();
          break;
        }
      }
    const json *track_node = nullptr;
    if (track.empty()) {
      track = "$new:track";
      json add = {{"op", "add"}, {"path", seq + "/tracks/$new:track"}, {"value", {{"kind", "video"}, {"name", "V1"}}}};
      if (const json order = sequence.value("track_order", json::array()); !order.empty())
        add["anchor"] = {{"before", order[0]}};
      ops.push_back(std::move(add));
    } else if (const auto t = tracks.find(track); t != tracks.end()) {
      track_node = &*t;
    } else {
      return fail(ErrorCode::UnknownId, "P_UNKNOWN_ID", "The sequence has no track \"" + track + "\".");
    }

    // Where: the end of the track, and the clip that ends there (what "start from the clip before" means).
    Rational end = Rational::from_int(0);
    std::string before;
    if (track_node && track_node->contains("clips"))
      for (auto c = (*track_node)["clips"].begin(); c != (*track_node)["clips"].end(); ++c) {
        const json timing = c->value("timing", json::object());
        const auto in = Rational::parse(timing.value("record_in", std::string("0")));
        const auto dur = Rational::parse(timing.value("duration", std::string("0")));
        const auto out = in && dur ? add(*in, *dur) : Result<Rational>(Rational::from_int(0));
        if (out && compare(*out, end) > 0) {
          end = *out;
          before = c.key();
        }
      }
    json inputs = json::object(); // what the workflow exposes: not every workflow has a prompt or a seed
    if (face.contains("prompt"))
      inputs["prompt"] = *prompt;
    if (face.contains("seed"))
      inputs["seed"] = seed;
    std::string start_reference;
    if (const std::string from = params.value("start_from", std::string()); !from.empty()) {
      const std::string source = from == "previous" ? before : from;
      const doc::NodeRef *ref = source.empty() ? nullptr : pr->doc.find(source);
      if (!ref || ref->node->value("media_ref", json::object()).value("type", std::string()) != "workflow")
        return fail(ErrorCode::InvalidArgument, "G_CLIP_LINK",
                    from == "previous" ? "There is no generative clip before this one on the track to start from."
                                       : "\"" + from + "\" is not a generative clip.",
                    {}, "Add the first clip without start_from, or name a generative clip.");
      if (decl && !decl->takes("start_image"))
        return fail(ErrorCode::InvalidArgument, "G_SETTING", "The model " + decl->id + " cannot start from a picture.", {},
                    "Pick a model that accepts a start picture.");
      start_reference = from == "previous" ? "previous" : from;
    }

    // Starting on the last frame of another clip is two more nodes in the clip's own workflow, a Clip Reference and a Get Frame node.
    if (!start_reference.empty() && !gen::start_from(instance, start_reference))
      return fail(ErrorCode::InvalidArgument, "G_SETTING", "This workflow has no start picture to start on a clip's last frame.", {},
                  "Use a workflow with a node that takes a start_image, exposed as \"start_image\".");
    std::string name = params.value("name", std::string());
    if (name.empty()) { // "Shot N": the next number no generative clip of the project has. A prompt makes a poor name: it
      int next = 1;     // often starts with the style, and any cut of it reads as broken.
      for (const gen::ClipIn &c : gen_clips(*pr))
        if (c.name.size() > 5 && c.name.rfind("Shot ", 0) == 0 && c.name.find_first_not_of("0123456789", 5) == std::string::npos)
          next = std::max(next, std::atoi(c.name.c_str() + 5) + 1);
      name = "Shot " + std::to_string(next);
    }
    char length[32];
    std::snprintf(length, sizeof length, "%.3fs", seconds);
    // The model's grid rarely gives the canvas's exact size (1264 x 704 for 1280 x 720): the clip is scaled to cover the
    // canvas, losing a sliver at two edges rather than showing a border.
    // Scale 1 is the picture fitted inside the canvas, so the factor is how much more it takes to cover it.
    // Where: the place asked for ("at", any form parse_time reads: "12.5s", "300@24", a timecode), else the end of the
    // track. A place that is taken by another clip of the track moves right, to the end of that clip: clips' times are
    // exact fractions of a second, so this is settled here and not on an editor's rounded frames.
    std::string record_in = end.to_string();
    if (params.contains("at")) {
      ATM_TRY(Rational at, parse_time(params["at"]));
      const auto len = Rational::make(std::llround(seconds * 1000.0), 1000);
      if (len && track_node && track_node->contains("clips"))
        for (bool moved = true; moved;) {
          moved = false;
          for (const auto &c : (*track_node)["clips"]) {
            const json timing = c.value("timing", json::object());
            const auto in = Rational::parse(timing.value("record_in", std::string("0")));
            const auto dur = Rational::parse(timing.value("duration", std::string("0")));
            if (!in || !dur)
              continue;
            const auto out = add(*in, *dur), at_end = add(at, *len);
            if (out && at_end && compare(at, *out) < 0 && compare(*in, *at_end) < 0) {
              at = *out;
              moved = true;
            }
          }
        }
      record_in = at.to_string();
    }
    const double across = cw / double(width), down = ch / double(height);
    const double fill = std::round(std::max(across, down) / std::min(across, down) * 10000.0) / 10000.0;
    ops.push_back({{"op", "add"},
                   {"path", track + "/clips/$new:clip"},
                   {"value",
                    {{"name", name},
                     {"timing", {{"record_in", record_in}, {"duration", length}, {"source_in", "0"}}},
                     {"media_ref", {{"type", "workflow"}, {"workflow", std::move(instance)}, {"inputs", std::move(inputs)}, {"width", width}, {"height", height}}},
                     {"transform", {{"position", {0.5, 0.5}}, {"scale", {fill, fill}}, {"opacity", 1}}}}}});
    ATM_TRY(json applied, project_patch({{"project", project_ref}, {"patch", {{"ops", std::move(ops)}, {"label", "Add generative clip"}}}}));
    const json &ids = applied["id_map"];
    return json{{"clip", ids.value("$new:clip", std::string())},
                {"track", ids.value("$new:track", track)}, {"width", width}, {"height", height}, {"seconds", seconds},
                {"seed", seed}, {"revision", applied["revision"]}};
  }

  // Publishes a clip's own workflow to the project's library: a new Clip Workflow, a copy with IDs of its own, that the Generate
  // panel shows as a card. The clip keeps its own; the values the clip gives its inputs are not part of it.
  Result<json> gen_save_to_library(const json &params) {
    ATM_TRY(Project *pr, project(params));
    ATM_TRY(const std::string *clip, string_param(params, "clip"));
    const json &workflow = instance_of(*pr, *clip);
    if (workflow.empty() || !workflow.contains("nodes"))
      return fail(ErrorCode::UnknownId, "G_WORKFLOW", "\"" + *clip + "\" is not a generative clip with a workflow of its own.", {},
                  "Pass the ID of a clip made by gen.create_clip.");
    json copy = gen::fresh_copy(workflow, std::string());
    copy["name"] = params.value("name", workflow.value("name", std::string("Workflow")));
    ATM_TRY(json applied, project_patch({{"project", to_utf8(pr->dir)},
                                         {"patch", {{"ops", json::array({{{"op", "add"}, {"path", pr->doc.root().value("id", std::string()) + "/workflows/$new:w"}, {"value", std::move(copy)}}})},
                                                    {"label", "Save to library"}}}}));
    return json{{"workflow", applied["id_map"].value("$new:w", std::string())}, {"name", params.value("name", workflow.value("name", std::string("Workflow")))},
                {"revision", applied["revision"]}};
  }

  // Puts a clip's workflow back to the Clip Workflow it was copied from (its "source": a built-in Shot, or a library workflow as
  // it is now). The clip's values for inputs the original does not have are dropped with the rest of its edits; all of it is
  // one edit, and can be undone.
  Result<json> gen_reset_clip(const json &params) {
    ATM_TRY(Project *pr, project(params));
    ATM_TRY(const std::string *clip, string_param(params, "clip"));
    const doc::NodeRef *ref = pr->doc.find(*clip);
    const json media = ref ? ref->node->value("media_ref", json::object()) : json::object();
    if (media.value("type", std::string()) != "workflow")
      return fail(ErrorCode::UnknownId, "G_WORKFLOW", "\"" + *clip + "\" is not a generative clip.", {}, "Pass the ID of a generative clip.");
    const std::string source = media.value("workflow", json::object()).value("source", std::string());
    json fresh;
    if (source.rfind("cwf_", 0) == 0) {
      const json &library = library_of(*pr);
      if (const auto wf = library.find(source); wf != library.end() && wf->is_object())
        fresh = gen::fresh_copy(*wf, source);
    } else {
      fresh = gen::instantiate(source);
    }
    if (fresh.is_null() || fresh.empty())
      return fail(ErrorCode::NotFound, "G_SOURCE", "The Clip Workflow of clip " + *clip + " (\"" + source + "\") is not in the Template Library any more.", {},
                  "Save its workflow to the library again, or keep the clip's own.");
    json ops = json::array();
    const json &face = fresh.value("exposed", json::object()).value("inputs", json::object());
    const json held = media.value("inputs", json::object());
    for (auto it = held.begin(); it != held.end(); ++it) // values the original has no input for
      if (!face.contains(it.key()))
        ops.push_back({{"op", "remove"}, {"path", *clip + "/media_ref/inputs/" + it.key()}});
    // The workflow holds collections, which are not taken away whole: what is in them goes one by one, then the copy is put in.
    const std::string at = *clip + "/media_ref/workflow";
    const json old = media.value("workflow", json::object());
    const json old_links = old.value("links", json::object()), old_nodes = old.value("nodes", json::object());
    const json old_exposed = old.value("exposed", json::object());
    for (auto it = old_links.begin(); it != old_links.end(); ++it)
      ops.push_back({{"op", "remove"}, {"path", it.key()}});
    for (auto it = old_nodes.begin(); it != old_nodes.end(); ++it)
      ops.push_back({{"op", "remove"}, {"path", it.key()}});
    for (const char *side : {"inputs", "outputs"}) {
      const json entries = old_exposed.value(side, json::object());
      for (auto it = entries.begin(); it != entries.end(); ++it)
        ops.push_back({{"op", "remove"}, {"path", at + "/exposed/" + side + "/" + it.key()}});
    }
    if (old_exposed.contains("primary"))
      ops.push_back({{"op", "remove"}, {"path", at + "/exposed/primary"}});
    ops.push_back({{"op", old.contains("name") ? "replace" : "add"}, {"path", at + "/name"}, {"value", fresh.value("name", std::string("Workflow"))}});
    ops.push_back({{"op", old.contains("source") ? "replace" : "add"}, {"path", at + "/source"}, {"value", fresh.value("source", std::string())}});
    for (const char *collection : {"nodes", "links"})
      for (auto it = fresh[collection].begin(); it != fresh[collection].end(); ++it)
        ops.push_back({{"op", "add"}, {"path", at + "/" + collection + "/" + it.key()}, {"value", *it}});
    const json made = fresh.value("exposed", json::object());
    for (const char *side : {"inputs", "outputs"}) {
      const json entries = made.value(side, json::object());
      for (auto it = entries.begin(); it != entries.end(); ++it)
        ops.push_back({{"op", "add"}, {"path", at + "/exposed/" + side + "/" + it.key()}, {"value", *it}});
    }
    if (made.contains("primary"))
      ops.push_back({{"op", "add"}, {"path", at + "/exposed/primary"}, {"value", made["primary"]}});
    return project_patch({{"project", to_utf8(pr->dir)}, {"patch", {{"ops", std::move(ops)}, {"label", "Reset workflow"}}}});
  }

  // A Preset: the input values of a clip kept under a name, for the Clip Workflow the clip was made from (its "source"). A Preset
  // of the same name and source is replaced.
  Result<json> gen_save_preset(const json &params) {
    ATM_TRY(Project *pr, project(params));
    ATM_TRY(const std::string *clip, string_param(params, "clip"));
    ATM_TRY(const std::string *name, string_param(params, "name"));
    const doc::NodeRef *ref = pr->doc.find(*clip);
    const json media = ref ? ref->node->value("media_ref", json::object()) : json::object();
    if (media.value("type", std::string()) != "workflow")
      return fail(ErrorCode::UnknownId, "G_WORKFLOW", "\"" + *clip + "\" is not a generative clip.", {}, "Pass the ID of a generative clip.");
    if (name->empty())
      return bad_param("name", "must not be empty");
    const std::string source = media.value("workflow", json::object()).value("source", std::string());
    const json &presets = pr->doc.root().contains("presets") && pr->doc.root()["presets"].is_object() ? pr->doc.root()["presets"] : json::object();
    json ops = json::array();
    for (auto it = presets.begin(); it != presets.end(); ++it)
      if (it->value("name", std::string()) == *name && it->value("source", std::string()) == source)
        ops.push_back({{"op", "remove"}, {"path", it.key()}});
    // Only what the clip's workflow has an input for; values the workflow does not list are not part of a Preset.
    json values = json::object();
    const json face = media.value("workflow", json::object()).value("exposed", json::object()).value("inputs", json::object());
    const json held = media.value("inputs", json::object());
    for (auto it = held.begin(); it != held.end(); ++it)
      if (face.contains(it.key()))
        values[it.key()] = *it;
    ops.push_back({{"op", "add"}, {"path", pr->doc.root().value("id", std::string()) + "/presets/$new:p"},
                   {"value", {{"name", *name}, {"source", source}, {"values", std::move(values)}}}});
    ATM_TRY(json applied, project_patch({{"project", to_utf8(pr->dir)}, {"patch", {{"ops", std::move(ops)}, {"label", "Save preset"}}}}));
    return json{{"preset", applied["id_map"].value("$new:p", std::string())}, {"revision", applied["revision"]}};
  }

  // A Preset put on a clip: its values go into the clip's inputs, for the inputs the clip's workflow has, in one edit. The rest are
  // reported as skipped.
  Result<json> gen_apply_preset(const json &params) {
    ATM_TRY(Project *pr, project(params));
    ATM_TRY(const std::string *clip, string_param(params, "clip"));
    ATM_TRY(const std::string *preset, string_param(params, "preset"));
    const doc::NodeRef *ref = pr->doc.find(*clip);
    const doc::NodeRef *pre = pr->doc.find(*preset);
    const json media = ref ? ref->node->value("media_ref", json::object()) : json::object();
    if (media.value("type", std::string()) != "workflow")
      return fail(ErrorCode::UnknownId, "G_WORKFLOW", "\"" + *clip + "\" is not a generative clip.", {}, "Pass the ID of a generative clip.");
    if (!pre || id_prefix(*preset) != "pre")
      return fail(ErrorCode::UnknownId, "G_PRESET", "\"" + *preset + "\" is not a Preset of this project.", {}, "List them with project.get on the project's ID: its \"presets\".");
    const json face = media.value("workflow", json::object()).value("exposed", json::object()).value("inputs", json::object());
    const json have = media.value("inputs", json::object());
    const json values = pre->node->value("values", json::object());
    json ops = json::array(), skipped = json::array();
    for (auto it = values.begin(); it != values.end(); ++it) {
      if (!face.contains(it.key())) {
        skipped.push_back(it.key());
        continue;
      }
      ops.push_back({{"op", have.contains(it.key()) ? "replace" : "add"}, {"path", *clip + "/media_ref/inputs/" + it.key()}, {"value", *it}});
    }
    if (ops.empty())
      return json{{"applied", 0}, {"skipped", std::move(skipped)}};
    ATM_TRY(json applied, project_patch({{"project", to_utf8(pr->dir)}, {"patch", {{"ops", std::move(ops)}, {"label", "Apply preset"}}}}));
    return json{{"applied", values.size() - skipped.size()}, {"skipped", std::move(skipped)}, {"revision", applied["revision"]}};
  }

  Result<json> gen_select_take(const json &params) {
    ATM_TRY(Project *pr, project(params));
    ATM_TRY(const std::string *clip, string_param(params, "clip"));
    ATM_TRY(const std::string *take, string_param(params, "take"));
    const doc::NodeRef *ref = pr->doc.find(*clip);
    const json media = ref ? ref->node->value("media_ref", json::object()) : json::object();
    const json takes = media.value("takes", json::object());
    if (!takes.contains(*take))
      return fail(ErrorCode::UnknownId, "G_TAKE", "Clip " + *clip + " has no Take \"" + *take + "\".", *clip + "/media_ref/takes",
                  "List the clip's Takes with project.get.");
    // The clip's inputs go back to what made that Take, so the clip is clean with it and clips after it follow.
    json ops = json::array({{{"op", media.contains("selected") ? "replace" : "add"}, {"path", *clip + "/media_ref/selected"}, {"value", *take}}});
    if (const json then = takes[*take].value("inputs", json::object()); then != media.value("inputs", json::object()))
      ops.push_back({{"op", media.contains("inputs") ? "replace" : "add"}, {"path", *clip + "/media_ref/inputs"}, {"value", then}});
    return project_patch({{"project", to_utf8(pr->dir)}, {"patch", {{"ops", std::move(ops)}, {"label", "Select take"}}}});
  }

  // ---- models.* (the model store: what can be downloaded, what is on disk) --------------------------------------

  // ATTOME_MODELS_DIR says where the models are for this run, whatever was chosen in the Models panel.
  static bool models_dir_fixed() {
    const char *dir = std::getenv("ATTOME_MODELS_DIR");
    return dir && *dir;
  }
  fs::path models_dir() const {
    if (!chosen_models_dir.empty() && !models_dir_fixed())
      return chosen_models_dir;
    return cfg.models_dir.empty() ? models::default_models_dir() : to_path(cfg.models_dir);
  }

  static bool same_folder(const fs::path &a, const fs::path &b) {
    std::error_code ec;
    return fs::weakly_canonical(a, ec) == fs::weakly_canonical(b, ec);
  }
  bool knows_folder(const fs::path &folder) const {
    return std::any_of(model_folders.begin(), model_folders.end(), [&](const fs::path &f) { return same_folder(f, folder); });
  }
  static int64_t free_bytes(const fs::path &dir) { // of the nearest folder that exists: the models folder may not, yet
    std::error_code ec;
    for (fs::path at = dir; !at.empty(); at = at.parent_path()) {
      if (const fs::space_info space = fs::space(at, ec); !ec)
        return int64_t(space.available);
      if (at == at.parent_path())
        break;
    }
    return -1;
  }
  json folders_json() const {
    json list = json::array();
    for (const fs::path &f : model_folders)
      list.push_back(to_utf8(f));
    return list;
  }
  Result<void> save_model_folders() {
    const fs::path path = settings_path();
    if (path.empty())
      return {};
    json all = settings();
    if (chosen_models_dir.empty())
      all.erase("models_dir");
    else
      all["models_dir"] = to_utf8(chosen_models_dir);
    all["model_folders"] = folders_json();
    ATM_CHECK(storage::make_dirs(path.parent_path()));
    return storage::atomic_write(path, all.dump(2) + "\n");
  }
  bool fetch_running() const {
    return std::any_of(jobs.begin(), jobs.end(),
                       [](const auto &j) { return j.second->kind == "models.fetch" && j.second->state.load() == Job::running; });
  }

  // How many of `files` are whole in `folder`.
  static int files_in(const std::vector<const models::CatalogFile *> &files, const fs::path &folder) {
    int n = 0;
    for (const models::CatalogFile *f : files)
      n += !models::find_file(*f, folder, {folder}).empty();
    return n;
  }

  // "I already have this model": the folder a person picked is looked at, and remembered when it holds model files.
  // People pick the folder they know, which is rarely the exact one: ComfyUI's own folder, its models folder, or the
  // subfolder a file is in all work.
  Result<json> models_locate(const json &params) {
    ATM_TRY(const std::string *folder, string_param(params, "folder"));
    const std::string id = params.value("id", std::string());
    const models::CatalogEntry *entry = id.empty() ? nullptr : models::find_entry(models::builtin_catalog(), id);
    if (!id.empty() && !entry)
      return fail(ErrorCode::NotFound, "M_UNKNOWN_MODEL", "There is no model \"" + id + "\" in the catalog.", {}, "List them with models.list.");
    std::error_code ec;
    const fs::path picked = to_path(*folder);
    if (folder->empty() || !fs::is_directory(picked, ec))
      return fail(ErrorCode::NotFound, "M_FOLDER", "\"" + *folder + "\" is not a folder.", {}, "Choose the folder the model files are in.");
    std::vector<const models::CatalogFile *> files;
    for (const models::CatalogEntry &e : models::builtin_catalog())
      if (!entry || &e == entry)
        for (const models::CatalogFile &f : e.files)
          if (std::none_of(files.begin(), files.end(), [&](const models::CatalogFile *have) { return have->path == f.path; }))
            files.push_back(&f);
    fs::path best;
    int found = 0;
    for (const fs::path &candidate : {picked, picked / "models", picked / "ComfyUI" / "models", picked.parent_path()})
      if (const int n = files_in(files, candidate); n > found) {
        found = n;
        best = candidate;
      }
    json out = {{"found", found}, {"of", int(files.size())}, {"added", false}};
    if (found == 0)
      return out;
    out["folder"] = to_utf8(best);
    if (!same_folder(best, models_dir()) && !knows_folder(best)) {
      model_folders.push_back(best);
      ATM_CHECK(save_model_folders());
      out["added"] = true;
    }
    if (entry) {
      int64_t missing = 0;
      const fs::path dir = models_dir();
      for (const models::CatalogFile &f : entry->files)
        missing += f.size - models::file_status(f, dir, model_folders).bytes;
      out["bytes_missing"] = missing;
      out["title"] = entry->title;
    }
    return out;
  }

  Result<json> models_forget_folder(const json &params) {
    ATM_TRY(const std::string *folder, string_param(params, "folder"));
    const size_t before = model_folders.size();
    std::erase_if(model_folders, [&](const fs::path &f) { return same_folder(f, to_path(*folder)); });
    if (model_folders.size() != before)
      ATM_CHECK(save_model_folders());
    return json{{"folders", folders_json()}, {"removed", model_folders.size() != before}};
  }

  // Where downloads go from now on. What is in the old folder stays there and stays usable: it becomes one of the
  // folders that are looked in.
  Result<json> models_set_folder(const json &params) {
    ATM_TRY(const std::string *folder, string_param(params, "folder"));
    if (models_dir_fixed())
      return fail(ErrorCode::InvalidArgument, "M_FOLDER_FIXED", "The models folder is set by ATTOME_MODELS_DIR for this run.", {},
                  "Start Attome without ATTOME_MODELS_DIR to choose the folder here.");
    if (fetch_running())
      return fail(ErrorCode::InvalidArgument, "M_BUSY", "A model is being downloaded.", {},
                  "Stop the download, or wait for it, then change the folder.");
    const fs::path old = models_dir();
    const fs::path next = folder->empty() ? fs::path() : to_path(*folder);
    std::error_code ec;
    if (!next.empty()) {
      fs::create_directories(next, ec);
      if (!fs::is_directory(next, ec))
        return fail(ErrorCode::IoError, "M_DISK", "The folder " + *folder + " cannot be used.", {}, "Choose a folder you can save files in.");
    }
    chosen_models_dir = next;
    const fs::path now = models_dir();
    std::erase_if(model_folders, [&](const fs::path &f) { return same_folder(f, now); });
    if (!same_folder(old, now) && fs::is_directory(old, ec) && !fs::is_empty(old, ec) && !knows_folder(old))
      model_folders.push_back(old);
    ATM_CHECK(save_model_folders());
    return json{{"models_dir", to_utf8(now)}, {"free_bytes", free_bytes(now)}, {"folders", folders_json()}};
  }

  Result<json> models_list(const json &) {
    const fs::path dir = models_dir();
    json entries = json::array();
    for (const models::CatalogEntry &e : models::builtin_catalog()) {
      json files = json::array();
      int64_t on_disk = 0;
      bool all = true;
      for (const models::CatalogFile &f : e.files) {
        const models::FileStatus st = models::file_status(f, dir, model_folders);
        on_disk += st.bytes;
        all = all && st.state == models::FileState::installed;
        files.push_back({{"path", f.path},
                         {"size", f.size},
                         {"bytes", st.bytes},
                         {"state", st.state == models::FileState::installed ? "installed" : st.state == models::FileState::partial ? "partial" : "missing"}});
      }
      std::string job_id; // a download of this entry that is running now
      for (const auto &[id, job] : jobs)
        if (job->kind == "models.fetch" && job->output == e.id && job->state.load() == Job::running)
          job_id = id;
      json entry = {{"id", e.id},       {"title", e.title},       {"kind", e.kind},     {"licence", e.licence},
                    {"licence_url", e.licence_url}, {"notes", e.notes}, {"size", e.size()},   {"bytes", on_disk},
                    {"state", all ? "installed" : !job_id.empty() ? "downloading" : on_disk > 0 ? "partial" : "missing"},
                    {"files", std::move(files)}};
      if (!job_id.empty())
        entry["job_id"] = job_id;
      entries.push_back(std::move(entry));
    }
    return json{{"models_dir", to_utf8(dir)}, {"free_bytes", free_bytes(dir)}, {"folders", folders_json()},
                {"folder_fixed", models_dir_fixed()}, {"entries", std::move(entries)}};
  }

  Result<json> models_fetch(const json &params) {
    ATM_TRY(const std::string *id, string_param(params, "id"));
    const models::CatalogEntry *entry = models::find_entry(models::builtin_catalog(), *id);
    if (!entry) {
      std::string ids;
      for (const models::CatalogEntry &e : models::builtin_catalog())
        ids += (ids.empty() ? "" : ", ") + e.id;
      return fail(ErrorCode::NotFound, "M_UNKNOWN_MODEL", "There is no model \"" + *id + "\" in the catalog.", {}, "Use one of: " + ids + ".");
    }
    for (const auto &[job_id, job] : jobs)
      if (job->kind == "models.fetch" && job->output == entry->id && job->state.load() == Job::running)
        return json{{"job_id", job_id}, {"id", entry->id}, {"bytes_total", entry->size()}, {"already_running", true}};
    const fs::path dir = models_dir();
    // Room on the disk for what is still missing, before any byte is fetched.
    int64_t missing = 0;
    for (const models::CatalogFile &f : entry->files)
      missing += f.size - models::file_status(f, dir, model_folders).bytes;
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::space_info space = fs::space(dir, ec);
    if (!ec && int64_t(space.available) < missing)
      return fail(ErrorCode::IoError, "M_DISK", entry->title + " needs " + std::to_string(missing / 1000000000) + " GB more, and " +
                                                    to_utf8(dir) + " has " + std::to_string(int64_t(space.available) / 1000000000) + " GB free.",
                  {}, "Free some space, or choose a folder on another drive in the Models panel.");
    auto job = std::make_shared<Job>();
    job->id = new_id("job");
    job->kind = "models.fetch";
    job->output = entry->id;
    job->units_total.store(entry->size());
    jobs[job->id] = job;
    job->thread = std::thread(run_fetch, job, *entry, dir, model_folders, cfg.transport ? cfg.transport : std::shared_ptr<net::Transport>(net::system_transport()));
    return json{{"job_id", job->id}, {"id", entry->id}, {"bytes_total", entry->size()}, {"bytes_missing", missing}, {"models_dir", to_utf8(dir)}};
  }

  Result<std::shared_ptr<Job>> job_param(const json &params) {
    ATM_TRY(const std::string *id, string_param(params, "job_id"));
    const auto it = jobs.find(*id);
    if (it == jobs.end())
      return fail(ErrorCode::NotFound, "R_NO_JOB", "There is no job \"" + *id + "\".");
    return it->second;
  }

  Result<json> jobs_get(const json &params) {
    ATM_TRY(std::shared_ptr<Job> job, job_param(params));
    static constexpr const char *kStates[] = {"running", "done", "failed", "cancelled"};
    const int state = job->state.load();
    const int64_t done = job->units_done.load(), total = job->units_total.load();
    json out = {{"job", job->id},
                {"kind", job->kind},
                {"state", kStates[state]},
                {"progress", total > 0 ? double(done) / double(total) : 0.0},
                {"frames_done", done},
                {"frames_total", total},
                {"units_done", done},
                {"units_total", total},
                {"unit", job->kind == "models.fetch" ? "bytes" : job->kind == "gen.run" ? "steps" : "frames"},
                {"output", job->output}};
    std::lock_guard lock(job->mutex);
    const double seconds = state == Job::running
                               ? std::chrono::duration<double>(Clock::now() - job->started).count()
                               : job->seconds;
    out["seconds"] = seconds;
    out["fps"] = seconds > 0.0 ? double(done) / seconds : 0.0;
    if (job->kind == "models.fetch")
      out["bytes_per_second"] = seconds > 0.0 ? double(job->fetched.load()) / seconds : 0.0;
    if (state == Job::failed)
      out["error"] = error_to_json(job->error);
    if (!job->warning.empty())
      out["warning"] = job->warning;
    if (!job->detail.empty())
      out["detail"] = job->detail;
    if (!job->result.is_null())
      out["result"] = job->result;
    if (!job->encoder.empty())
      out["encoder"] = job->encoder;
    if (state != Job::running && job->thread.joinable())
      job->thread.join();
    return out;
  }

  Result<json> jobs_cancel(const json &params) {
    ATM_TRY(std::shared_ptr<Job> job, job_param(params));
    job->cancel.store(true);
    return json{{"job", job->id}, {"cancelling", true}};
  }

  Result<json> profile_get(const json &params) {
    json snap = prof::snapshot();
    if (params.value("reset", false))
      prof::reset();
    return snap;
  }

  Result<json> profile_reset(const json &) {
    prof::reset();
    return json{{"reset", true}};
  }

  Result<json> profile_set(const json &params) {
    const auto on = params.find("enabled");
    if (on == params.end() || !on->is_boolean())
      return bad_param("enabled", "is required and must be true or false");
    prof::set_enabled(on->get<bool>());
    return json{{"enabled", prof::enabled()}};
  }
};

// Parameter schemas are JSON Schema (the MCP inputSchema); "project" is the .attome folder or a prj_ ID.
const Engine::Impl::Tool Engine::Impl::kTools[] = {
    {"project.create", "core", true, "Create a new .attome project folder with one empty Sequence.",
     R"({"type":"object","properties":{
       "path":{"type":"string","description":"Folder to create; \".attome\" is added when missing"},
       "name":{"type":"string"},
       "rate":{"type":"string","description":"Frame rate, e.g. \"30\", \"25\" or \"30000/1001\". Default 30"},
       "canvas":{"type":"object","properties":{"width":{"type":"integer"},"height":{"type":"integer"}},
                 "description":"Default 1920 x 1080"}},
       "required":["path"]})",
     &Impl::project_create},
    {"project.inspect", "core", false,
     "Summary of a project: sequences, tracks and (level \"tracks\") every clip with its ID, start and duration.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "level":{"type":"string","enum":["summary","tracks"],"description":"\"tracks\" also lists the clips"},
       "max_items":{"type":"integer","description":"Clips listed per track, default 200"}},
       "required":["project"]})",
     &Impl::project_inspect},
    {"project.get", "core", false, "One object (sequence, track, clip …) by Stable ID, with all its fields.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},"id":{"type":"string"}},"required":["project","id"]})",
     &Impl::project_get},
    {"project.patch", "core", true,
     "Low-level edit with an ID-addressed Patch; prefer timeline.edit, and use this for what it does not cover. "
     "All ops apply or none do. guide.get shows the "
     "exact shapes of tracks, clips, text, dissolves, keyframes, effects and sound (topics: clips, text, dissolves, "
     "keyframes, effects, audio, times).\n"
     "Ops: add, remove, replace, move, insert_order, remove_order, test. A path is \"<StableID>/<field>[/…]\": add "
     "{\"op\":\"add\",\"path\":\"<track_id>/clips/$new:c1\",\"value\":{…}}, change {\"op\":\"replace\",\"path\":"
     "\"<clip_id>/timing/duration\",\"value\":\"3s\"}, delete {\"op\":\"remove\",\"path\":\"<clip_id>\"}. "
     "$new:<name> placeholders become Stable IDs (returned in id_map) and later ops of the same patch may use them. "
     "A refused patch says which rule failed and how to fix it. dry_run checks without changing anything.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "patch":{"type":"object","properties":{
         "ops":{"type":"array","items":{"type":"object","properties":{
           "op":{"type":"string","enum":["add","remove","replace","move","insert_order","remove_order","test"]},
           "path":{"type":"string"},"value":{},"to":{"type":"string"}},"required":["op","path"]}},
         "label":{"type":"string","description":"Short description shown in the history"},
         "base_revision":{"type":"integer","description":"Refuse the patch when the project moved past this revision"}},
         "required":["ops"]},
       "dry_run":{"type":"boolean"},
       "task_id":{"type":"string","description":"Groups several edits into one task"}},
       "required":["project","patch"]})",
     &Impl::project_patch},
    {"timeline.edit", "core", true,
     "Edit the timeline with high-level ops, all applied together as one undoable step: add_track, add_clip, add_text, "
     "add_adjustment, add_transition, delete, ripple_delete, move, trim, split, slip, roll, slide, add_effect, "
     "remove_effect, set_effect_enabled, link, unlink, set_property. A video with sound becomes linked picture and "
     "sound clips that edits keep together. "
     "Clip defaults are "
     "worked out for you (append to the track, the rest of the file, a Titles track for text, an Effects track under "
     "it for blur). Give an op \"id\": \"$new:name\" and later ops can use that name. guide.get topic \"timeline\" "
     "has every op's fields and a complete example. Times accept \"2.5s\", \"75@30\" or timecode.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "ops":{"type":"array","items":{"type":"object","properties":{
         "op":{"type":"string","enum":["add_track","add_clip","add_text","add_adjustment","add_transition","delete",
                                       "ripple_delete","move","trim","split","slip","roll","slide","add_effect","remove_effect",
                                       "set_effect_enabled","link","unlink","set_property"]},
         "id":{"type":"string","description":"$new:name for what this op creates"}},"required":["op"]}},
       "sequence":{"type":"string"},"label":{"type":"string"},"dry_run":{"type":"boolean"},
       "task_id":{"type":"string","description":"Groups several calls into one task"}},
       "required":["project","ops"]})",
     &Impl::timeline_edit},
    {"media.import", "core", true,
     "Add media files to the project as assets (probed for size, length and sound). Returns their asset IDs for "
     "timeline.edit add_clip. Importing a path again returns the same asset.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "paths":{"type":"array","items":{"type":"string"},"description":"Absolute paths of video, image or sound files"},
       "task_id":{"type":"string"}},
       "required":["project","paths"]})",
     &Impl::media_import},
    {"guide.get", "core", false,
     "How to write the project: the shapes of tracks and clips, text, dissolves, keyframe animation, effects, sound "
     "and times, with examples ready to adapt. Read it before the first project.patch.",
     R"({"type":"object","properties":{"topic":{"type":"string","enum":["timeline","clips","text","dissolves","keyframes","effects","audio","times"],
       "description":"Leave out to get every topic"}}})",
     &Impl::guide_get},
    {"project.undo", "core", true, "Undo the last edit (steps: N).",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},"steps":{"type":"integer"}},"required":["project"]})",
     &Impl::project_undo},
    {"project.redo", "project", true, "Redo along the most recent branch of the undo tree.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},"steps":{"type":"integer"}},"required":["project"]})",
     &Impl::project_redo},
    {"project.validate", "core", false, "Check the whole project against the semantic rules.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"}},"required":["project"]})", &Impl::project_validate},
    {"project.save", "project", true, "Write project.json now.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"}},"required":["project"]})", &Impl::project_save},
    {"project.close", "project", true, "Save and close a project.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"}},"required":["project"]})", &Impl::project_close},
    {"history.list", "project", false, "Edits (ChangeSets) of the current epoch and the HEAD.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},"limit":{"type":"integer"}},"required":["project"]})",
     &Impl::history_list},
    {"time.parse", "project", false, "Convert any accepted time spelling to its canonical forms.",
     R"({"type":"object","properties":{"value":{"type":["string","object"],"description":"\"12.5s\", \"375@30\" (frames@rate), SMPTE \"00:00:12:15\" or {num,den} seconds"},"rate":{"type":"string"}},"required":["value"]})",
     &Impl::time_parse},
    {"media.probe", "core", false,
     "Size, frame rate, duration and audio format of a media file. Still pictures (PNG, JPEG, BMP, GIF, TGA) report "
     "image: true and their size, and have no duration.",
     R"({"type":"object","properties":{"path":{"type":"string","description":"Absolute path of the file"}},
       "required":["path"]})",
     &Impl::media_probe},
    {"see.frames", "core", false,
     "Render frames of the sequence as JPEG pictures, to check an edit by eye. Times past the end show the last "
     "frame. The files are replaced by the next see.* call.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "times":{"type":"array","items":{"type":["string","object"],"description":"\"12.5s\", \"375@30\" (frames@rate), SMPTE \"00:00:12:15\" or {num,den} seconds"},"minItems":1,"maxItems":16},
       "height":{"type":"integer","description":"Picture height in pixels, default 540"},
       "sequence":{"type":"string","description":"Sequence ID, default the first"}},
       "required":["project","times"]})",
     &Impl::see_frames},
    {"see.contact_sheet", "core", false,
     "One JPEG with evenly spaced frames of the whole sequence in a grid, each labelled with its timecode. The "
     "quickest way to review a cut before rendering it.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "count":{"type":"integer","description":"Number of frames, 1 to 48, default 12"},
       "columns":{"type":"integer","description":"Default 4"},
       "tile_height":{"type":"integer","description":"Pixels per frame, default 180"},
       "sequence":{"type":"string"}},
       "required":["project"]})",
     &Impl::see_contact_sheet},
    {"render.sequence", "core", false,
     "Export a sequence to an H.264 + AAC .mp4 as a background job. Returns job_id at once; follow it with jobs.get.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "output":{"type":"string","description":"Absolute path of the .mp4 to write"},
       "height":{"type":"integer","description":"Output height, default the canvas height"},
       "bitrate":{"type":"integer"},"audio":{"type":"boolean"},
       "overwrite":{"type":"boolean","description":"Default true"},"sequence":{"type":"string"}},
       "required":["project","output"]})",
     &Impl::render_sequence},
    {"gen.status", "gen", false,
     "The generative clips of a project, in the order they would run: each one's state (clean, dirty with the reason, empty, locked), "
     "its Takes, and whether it can run on this computer: a clip is not ready while a node of its "
     "workflow has no model chosen (G_MODEL_UNSET), a model this version does not know (G_MODEL_UNKNOWN) or one that is not "
     "installed (G_MODEL_MISSING, with the model's title and the bytes still to download: start it with models.fetch).",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"}},"required":["project"]})", &Impl::gen_status},
    {"gen.run", "gen", true,
     "Generate clips as a background job (units are steps; follow it with jobs.get, stop it with jobs.cancel). scope: \"dirty\" "
     "(default: every clip whose Take is not what its inputs ask for, or that has none), \"all\", \"selected\" (clips: [...], plus what "
     "they need upstream that is itself dirty) or \"selected_and_after\" (and every clip that starts from them). Clips run in "
     "dependency order; a step whose result is in the cache is not run again; a locked clip never runs. new_take: true makes "
     "another Take of the named clips with the next seed. dry_run: true returns the plan only: for every clip its state (clean, "
     "dirty, empty, locked), the reason, whether it runs, and the number of steps. A finished clip gets a Take and selects it.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "scope":{"type":"string","enum":["dirty","all","selected","selected_and_after"]},
       "clips":{"type":"array","items":{"type":"string"}},"new_take":{"type":"boolean"},"dry_run":{"type":"boolean"}},
       "required":["project"]})",
     &Impl::gen_run},
    {"gen.engines", "gen", false,
     "The engines that can run models here (the user's ComfyUI when its address is set), each with whether it answers, its version and device.",
     "", &Impl::gen_engines},
    {"gen.set_comfyui", "gen", false,
     "Set the address of the user's own ComfyUI (for example http://127.0.0.1:8188), used as an engine; \"\" stops using it. "
     "The address is remembered. Returns whether ComfyUI answers there, its version and device.",
     R"({"type":"object","properties":{"address":{"type":"string"}},"required":["address"]})", &Impl::gen_set_comfyui},
    {"gen.models", "gen", false,
     "The models a generative clip can be made with: each with its title, whether its files are installed, whether an engine here "
     "runs it, the optional inputs it accepts and the clip lengths it makes.",
     "", &Impl::gen_models},
    {"gen.nodes", "gen", false,
     "What a Clip Workflow is built from: the node kinds with their input and output ports (name, type, required), and every "
     "model with the kinds it runs, its settings (type, range, options, default), the optional inputs it accepts, whether "
     "its files are installed and whether an engine here runs each kind.",
     "", &Impl::gen_nodes},
    {"gen.create_clip", "gen", true,
     "Add a generative clip: a prompt and a model (or a workflow of the project's library). It goes at the end of the picture track (or at: a time), gets its own copy "
     "of the built-in Shot workflow for that model (its Instance: change it and no other clip changes), and is not generated yet: run gen.run. start_from: \"previous\" (or a clip ID) makes "
     "it start on the last frame of that clip, with a Clip Reference and a Get Frame node. The size is the canvas's shape at about 0.9 megapixels on the model's grid (a Project node), the length is the clip's Duration (a Clip node).",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "prompt":{"type":"string"},"model":{"type":"string","description":"An id from gen.models"},
       "workflow":{"type":"string","description":"Instead of a model: the ID of a Clip Workflow of the project's library, a key of its workflows"},"seconds":{"type":"number"},
       "seed":{"type":"integer"},"name":{"type":"string"},
       "start_from":{"type":"string","description":"\"previous\" or a generative clip's ID"},"at":{"type":"string"},
       "track":{"type":"string"},"sequence":{"type":"string"}},"required":["project","prompt"]})",
     &Impl::gen_create_clip},
    {"gen.save_to_library", "gen", true,
     "Publish a generative clip's own workflow to the project's library as a new Clip Workflow (a card in the Generate panel). "
     "The clip keeps its own; its input values are not part of the copy. name: the library's name for it (default: the workflow's).",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "clip":{"type":"string"},"name":{"type":"string"}},"required":["project","clip"]})",
     &Impl::gen_save_to_library},
    {"gen.reset_clip", "gen", true,
     "Put a generative clip's workflow back to the Clip Workflow it was copied from (its source): the built-in Shot, or the library "
     "workflow as it is now. One undoable edit; the clip's values for inputs the original does not have are dropped.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "clip":{"type":"string"}},"required":["project","clip"]})",
     &Impl::gen_reset_clip},
    {"gen.save_preset", "gen", true,
     "Keep a generative clip's input values under a name as a Preset of the Clip Workflow it was made from (a Preset of the same name "
     "and source is replaced). The project's \"presets\" hold them.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "clip":{"type":"string"},"name":{"type":"string"}},"required":["project","clip","name"]})",
     &Impl::gen_save_preset},
    {"gen.apply_preset", "gen", true,
     "Put a Preset's values on a generative clip, for the inputs its workflow has, in one undoable edit. Returns how many were set and "
     "which were skipped (inputs the clip's workflow lacks).",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "clip":{"type":"string"},"preset":{"type":"string","description":"A pre_ ID"}},"required":["project","clip","preset"]})",
     &Impl::gen_apply_preset},
    {"gen.select_take", "gen", true,
     "Choose which Take of a generative clip plays. The clip's inputs go back to what made that Take; clips that start from it become dirty.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "clip":{"type":"string"},"take":{"type":"string"}},"required":["project","clip","take"]})",
     &Impl::gen_select_take},
    {"models.list", "models", false,
     "The models this version can download, with their size, licence and what is already on disk (installed, partial, downloading, missing); "
     "the folder downloads go to with its free space, and the other folders that are looked in.",
     "", &Impl::models_list},
    {"models.fetch", "models", false,
     "Download a model from the catalog as a background job (units are bytes; follow it with jobs.get, stop it with jobs.cancel). "
     "A stopped or interrupted download continues from where it was; every file is checked against its SHA-256 before it is put in place.",
     R"({"type":"object","properties":{"id":{"type":"string","description":"An id from models.list"}},"required":["id"]})", &Impl::models_fetch},
    {"models.locate", "models", false,
     "Use model files that are already on this computer instead of downloading them: give the folder they are in (a ComfyUI "
     "folder, its models folder, or a folder of the files themselves). Returns how many of the catalog's files were found there "
     "(of one model with id) and, with id, the bytes still to download. A folder that holds some is remembered and looked in from then on.",
     R"({"type":"object","properties":{"folder":{"type":"string"},"id":{"type":"string","description":"An id from models.list"}},"required":["folder"]})",
     &Impl::models_locate},
    {"models.forget_folder", "models", false, "Stop looking for model files in a folder given to models.locate. Nothing is deleted.",
     R"({"type":"object","properties":{"folder":{"type":"string"}},"required":["folder"]})", &Impl::models_forget_folder},
    {"models.set_folder", "models", false,
     "Choose the folder downloads go to (for example on a drive with more room); \"\" goes back to the default one. Models in the "
     "folder used before stay usable. The choice is remembered.",
     R"({"type":"object","properties":{"folder":{"type":"string"}},"required":["folder"]})", &Impl::models_set_folder},
    {"jobs.get", "core", false, "State and progress of a job.",
     R"({"type":"object","properties":{"job_id":{"type":"string"}},"required":["job_id"]})", &Impl::jobs_get},
    {"jobs.cancel", "core", false, "Stop a running job.",
     R"({"type":"object","properties":{"job_id":{"type":"string"}},"required":["job_id"]})", &Impl::jobs_cancel},
    {"tools.list", "core", false, "The Tools this engine offers, with their parameter schemas.", "",
     &Impl::tools_list},
    {"daemon.hello", "daemon", false, "Protocol handshake.", "", &Impl::daemon_hello},
    {"daemon.status", "daemon", false, "Open projects, uptime and settings.", "", &Impl::daemon_status},
    {"daemon.shutdown", "daemon", true, "Save everything and stop the daemon.", "", &Impl::daemon_shutdown},
    {"profile.get", "daemon", false, "Zone timings of every engine thread. reset: true zeroes them afterwards.",
     R"({"type":"object","properties":{"reset":{"type":"boolean"}}})", &Impl::profile_get},
    {"profile.reset", "daemon", false, "Zero the profiler statistics.", "", &Impl::profile_reset},
    {"profile.set", "daemon", false, "Switch the profiler on or off at run time.",
     R"({"type":"object","properties":{"enabled":{"type":"boolean"}},"required":["enabled"]})", &Impl::profile_set},
};

Result<json> Engine::Impl::tools_list(const json &) {
  json list = json::array();
  for (const Tool &tool : kTools)
    list.push_back({{"name", tool.name},
                    {"group", tool.group},
                    {"mutating", tool.mutating},
                    {"summary", tool.summary},
                    {"params", *tool.params ? json::parse(tool.params) : json{{"type", "object"}}}});
  return json{{"tools", std::move(list)}};
}

Engine::Engine(EngineConfig config) : impl_(std::make_unique<Impl>()) {
  impl_->cfg = config;
  impl_->providers = config.providers;
  if (impl_->providers.empty() && std::getenv("ATTOME_MOCK_ENGINE"))
    impl_->providers.push_back(std::make_shared<MockProvider>());
  std::string comfyui = config.comfyui;
  if (const char *address = std::getenv("ATTOME_COMFYUI"); comfyui.empty() && address)
    comfyui = address;
  if (comfyui.empty())
    comfyui = impl_->settings().value("comfyui", std::string());
  impl_->set_comfyui(comfyui);
  if (!Impl::models_dir_fixed()) { // a run that names its models folder is not mixed with the user's own folders
    const json saved = impl_->settings();
    if (const std::string dir = saved.value("models_dir", std::string()); !dir.empty())
      impl_->chosen_models_dir = to_path(dir);
    for (const json &f : saved.value("model_folders", json::array()))
      if (f.is_string() && !f.get_ref<const std::string &>().empty())
        impl_->model_folders.push_back(to_path(f.get<std::string>()));
  }
  for (const Impl::Tool &tool : Impl::kTools)
    impl_->by_name.emplace(tool.name, &tool);
  // What each catalog model declares, for the validator: known whether or not the files are on this machine.
  for (const models::CatalogEntry &entry : models::builtin_catalog())
    if (entry.declares.is_object() && !gen::find_model(entry.id)) {
      json declaration = entry.declares;
      declaration["id"] = entry.id;
      if (auto model = gen::parse_model(declaration))
        gen::register_model(std::move(*model));
    }
}

Engine::~Engine() = default;

Result<json> Engine::call(std::string_view tool, const json &params) {
  const auto it = impl_->by_name.find(tool);
  if (it == impl_->by_name.end())
    return fail(ErrorCode::NotFound, "RPC_METHOD_NOT_FOUND", "There is no Tool named \"" + std::string(tool) + "\".",
                {}, "List the Tools with: attome tools");
  ATM_PROFILE_SCOPE(it->second->name);
  impl_->apply_finished();
  static const json empty = json::object();
  if (!params.is_object() && !params.is_null())
    return bad_param("params", "must be an object");
  try {
    return (impl_.get()->*(it->second->fn))(params.is_null() ? empty : params);
  } catch (const std::exception &e) { // third-party code must not throw across the module boundary
    return fail(ErrorCode::Internal, "E_INTERNAL", std::string("Internal error: ") + e.what(), {},
                "Please report this with the command you ran.");
  }
}

void Engine::save_all(std::chrono::milliseconds quiet) {
  const auto now = Clock::now();
  for (auto &[key, pr] : impl_->projects)
    if (pr->dirty && now - pr->last_change >= quiet)
      (void)impl_->save(*pr);
}

bool Engine::shutdown_requested() const { return impl_->shutdown; }

json rpc_dispatch(Engine &engine, const json &request) {
  if (request.is_array()) { // batch
    json out = json::array();
    for (const json &one : request)
      if (json r = rpc_dispatch(engine, one); !r.is_null())
        out.push_back(std::move(r));
    return out.empty() ? json(nullptr) : out;
  }
  json response = json::object();
  response["jsonrpc"] = "2.0";
  const auto method = request.is_object() ? request.find("method") : request.end();
  if (!request.is_object() || method == request.end() || !method->is_string()) {
    response["id"] = nullptr;
    response["error"] = {{"code", -32600}, {"message", "Invalid request: a string \"method\" is required."}};
    return response;
  }
  static const json no_params;
  const auto params = request.find("params");
  auto result = engine.call(method->get_ref<const std::string &>(), params == request.end() ? no_params : *params);
  const auto id = request.find("id");
  if (id == request.end()) // notification
    return nullptr;
  response["id"] = *id;
  if (result) {
    response["result"] = std::move(*result);
  } else {
    json error = error_to_json(result.error());
    if (result.error().rule == "RPC_METHOD_NOT_FOUND")
      error["code"] = -32601;
    response["error"] = std::move(error);
  }
  return response;
}

} // namespace atm::api
