#include "atm/api/engine.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "atm/base/hash.hpp"
#include "atm/base/id.hpp"
#include "atm/base/profiler.hpp"
#include "atm/base/time.hpp"
#include "atm/doc/document.hpp"
#include "atm/patch/history.hpp"
#include "atm/patch/patch.hpp"
#include "atm/render/render.hpp"
#include "atm/storage/file.hpp"

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
  std::atomic<int> state{running};
  std::atomic<int64_t> units_done{0}, units_total{0};
  std::atomic<bool> cancel{false};
  std::mutex mutex; // guards error, warning, seconds
  Error error;
  std::string warning;
  double seconds = 0.0;
  Clock::time_point started = Clock::now();
  std::thread thread;
};

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
    return json{{"ok", ok}, {"errors", std::move(errors)}, {"revision", pr->revision}};
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
    ATM_TRY(render::Composition comp, render::compile(pr->doc.root(), params.value("sequence", std::string())));
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
                {"output", job->output}};
    std::lock_guard lock(job->mutex);
    const double seconds = state == Job::running
                               ? std::chrono::duration<double>(Clock::now() - job->started).count()
                               : job->seconds;
    out["seconds"] = seconds;
    out["fps"] = seconds > 0.0 ? double(done) / seconds : 0.0;
    if (state == Job::failed)
      out["error"] = error_to_json(job->error);
    if (!job->warning.empty())
      out["warning"] = job->warning;
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

const Engine::Impl::Tool Engine::Impl::kTools[] = {
    {"project.create", "core", true, "Create a new .attome project folder with one empty Sequence.",
     &Impl::project_create},
    {"project.inspect", "core", false, "Summary of a project for people and agents. level: summary | tracks.",
     &Impl::project_inspect},
    {"project.get", "core", false, "One object by Stable ID.", &Impl::project_get},
    {"project.patch", "core", true,
     "Apply an ID-addressed Patch. Ops: add, remove, replace, move, insert_order, remove_order, test. Paths are "
     "<StableID>/<field>. Times accept \"12.5s\", SMPTE, \"frames@rate\" or {num,den}. dry_run previews and "
     "returns the normalized patch and its inverse.",
     &Impl::project_patch},
    {"project.undo", "core", true, "Undo the last ChangeSet (steps: N).", &Impl::project_undo},
    {"project.redo", "project", true, "Redo along the most recent branch of the undo tree.", &Impl::project_redo},
    {"project.validate", "core", false, "Check the whole project against the semantic rules.",
     &Impl::project_validate},
    {"project.save", "project", true, "Write project.json now.", &Impl::project_save},
    {"project.close", "project", true, "Save and close a project.", &Impl::project_close},
    {"history.list", "project", false, "ChangeSets of the current epoch and the HEAD.", &Impl::history_list},
    {"time.parse", "project", false, "Convert any accepted time spelling to its canonical forms.",
     &Impl::time_parse},
    {"media.probe", "core", false, "Size, frame rate, duration and audio format of a media file.",
     &Impl::media_probe},
    {"render.sequence", "core", false,
     "Export a sequence to an H.264 + AAC .mp4 as a background job. Params: project, output, height?, bitrate?, "
     "audio?. Returns job_id; follow it with jobs.get.",
     &Impl::render_sequence},
    {"jobs.get", "core", false, "State and progress of a job.", &Impl::jobs_get},
    {"jobs.cancel", "core", false, "Stop a running job.", &Impl::jobs_cancel},
    {"tools.list", "core", false, "The Tools this engine offers.", &Impl::tools_list},
    {"daemon.hello", "daemon", false, "Protocol handshake.", &Impl::daemon_hello},
    {"daemon.status", "daemon", false, "Open projects, uptime and settings.", &Impl::daemon_status},
    {"daemon.shutdown", "daemon", true, "Save everything and stop the daemon.", &Impl::daemon_shutdown},
    {"profile.get", "daemon", false, "Zone timings of every engine thread. reset: true zeroes them afterwards.",
     &Impl::profile_get},
    {"profile.reset", "daemon", false, "Zero the profiler statistics.", &Impl::profile_reset},
    {"profile.set", "daemon", false, "Switch the profiler on or off at run time.", &Impl::profile_set},
};

Result<json> Engine::Impl::tools_list(const json &) {
  json list = json::array();
  for (const Tool &tool : kTools)
    list.push_back({{"name", tool.name}, {"group", tool.group}, {"mutating", tool.mutating},
                    {"summary", tool.summary}});
  return json{{"tools", std::move(list)}};
}

Engine::Engine(EngineConfig config) : impl_(std::make_unique<Impl>()) {
  impl_->cfg = config;
  for (const Impl::Tool &tool : Impl::kTools)
    impl_->by_name.emplace(tool.name, &tool);
}

Engine::~Engine() = default;

Result<json> Engine::call(std::string_view tool, const json &params) {
  const auto it = impl_->by_name.find(tool);
  if (it == impl_->by_name.end())
    return fail(ErrorCode::NotFound, "RPC_METHOD_NOT_FOUND", "There is no Tool named \"" + std::string(tool) + "\".",
                {}, "List the Tools with: attome tools");
  ATM_PROFILE_SCOPE(it->second->name);
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
