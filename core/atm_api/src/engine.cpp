#include "atm/api/engine.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
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
         "- add_transition {between: [first, second], type? (dissolve | wipe | push | zoom), direction? (left | right | up | down, for a wipe or push: the side the new clip enters from), softness? (0.01..1, a wipe's edge, 0.1), amount? (0.05..2, a zoom's size, 0.5), duration? (1s), alignment? (center | start | end), make_room? (true: trim what the media lacks, see make_room)} - a dissolve, a wipe (an edge crosses the picture), a push (the old clip slides away, the new one follows) or a zoom (the old picture grows while the new one settles into place); "
         "the clips must touch, and center needs half the duration of spare media on each side of the cut\n"
         "- make_room {between: [first, second], duration? (1s), alignment? (center)} - trims what a transition of that length "
         "would lack: the end of the first clip and the start of the second come in by the missing media, the second clip moves up "
         "to meet the first, and every later clip of its track (and the clips linked to them) moves up by the same amount, so no "
         "gap opens and the track gets shorter after the cut. Clips on other tracks that are not linked stay put. add_transition "
         "with \"make_room\": true does both in one step\n"
         "- add_track {kind (video | audio), name?, position? (top | bottom), below? / above? (track ID)}\n"
         "- delete {clip} or {transition}; ripple_delete {clip} (closes the gap); move {clip, to?, track?}; trim {clip, edge (in | out), to or delta}; "
         "split {clip, at}; slip {clip, delta} (shows another part of its file, stays in place); roll {between: "
         "[first, second], delta} (moves the cut between them); slide {clip, delta} (moves the clip between its "
         "neighbours, which give and take the time)\n"
         "- add_effect {target (clip), type? (gaussian_blur | color_grade | vignette), plus the effect's parameters: radius | brightness, contrast, saturation | strength, radius, softness} changes only that clip (a blur softens its edges "
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
         "below it while it plays. The effects: attome.gaussian_blur (radius), attome.color_grade (brightness -1..1, contrast -1..1, saturation 0..3, 1 = unchanged) and attome.vignette (strength 0..1, radius 0..1 where darkening starts, softness 0.01..1). Blur the video under a title for its "
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
    ATM_TRY(render::Composition comp, render::compile(pr.doc.root(), params.value("sequence", std::string())));
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
