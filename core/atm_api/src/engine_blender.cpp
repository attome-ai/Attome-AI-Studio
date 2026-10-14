// Blender as something an agent can use: blender.status, blender.install, blender.run, blender.inspect, blender.render.
//
// Blender is another program (GPL-3.0) that Attome runs headless (`blender -b`) as a child process; Attome never contains it. An agent writes
// Python for it (the skill `blender` says how, with a helper module); the scene lives in a .blend file, so every call is independent of the last
// one and nothing is held open between them. blender.install fetches Blender Foundation's own portable Windows release (one pinned version,
// checked against the SHA-256 that release publishes) when the computer has none; a Blender that is already there is used as it is.
#include "engine_impl.hpp"

#include <cstdio>
#include <fstream>
#include <thread>

#include "atm/models/models.hpp"
#include "atm/net/http.hpp"
#include "atm/skills/skills.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace atm::api {

namespace {

constexpr const char *kVersion = "5.2.2";
constexpr const char *kArchive = "blender-5.2.2-windows-x64.zip";
constexpr const char *kArchiveUrl = "https://download.blender.org/release/Blender5.2/blender-5.2.2-windows-x64.zip";
constexpr int64_t kArchiveSize = 404453484; // as download.blender.org lists it (read 2026-10-14)
constexpr const char *kArchiveSha = "3849d17a682cba006075aaa3f3597ecb5c9c30ec31035b2e092c53e40679b535"; // blender-5.2.2.sha256 on the same site
constexpr const char *kFolder = "blender-5.2.2-windows-x64";

const char *kUnsupported = "Blender is run from Attome on Windows in this version.";

// What runs inside Blender. It reads spec.values beside it: the agent's code (code.py), whether to start from an empty scene, where to save the
// .blend, and what to render. It writes result.json: {ok, error, result, render}.
const char *kRunner = R"PY(
import json, os, sys, traceback, time
work = sys.argv[sys.argv.index("--") + 1]
sys.path.insert(0, work)
import bpy
spec = json.load(open(os.path.join(work, "spec.values"), encoding="utf8"))
out = {"ok": True, "error": None, "result": None, "render": None}
try:
    if spec.get("empty"):
        bpy.ops.wm.read_factory_settings(use_empty=True)
    import attome_blender as ab
    g = {"bpy": bpy, "ab": ab, "result": None, "__name__": "__attome__"}
    code_path = os.path.join(work, "code.py")
    if os.path.exists(code_path):
        exec(compile(open(code_path, encoding="utf8").read(), "your_code", "exec"), g)
    out["result"] = g.get("result")
    rs = spec.get("render")
    if rs:
        scene = bpy.context.scene
        r = scene.render
        if scene.camera is None:
            raise RuntimeError("The scene has no camera. Add one (ab.camera(...)) and make it the scene's camera.")
        wanted = {"eevee": "BLENDER_EEVEE", "cycles": "CYCLES", "workbench": "BLENDER_WORKBENCH"}.get(str(rs.get("engine", "")).lower(), "")
        ids = [e.identifier for e in bpy.types.RenderSettings.bl_rna.properties["engine"].enum_items]
        if wanted:
            r.engine = wanted if wanted in ids else next((i for i in ids if i.startswith("BLENDER_EEVEE")), r.engine)
        if rs.get("width"):
            r.resolution_x = int(rs["width"])
        if rs.get("height"):
            r.resolution_y = int(rs["height"])
        if rs.get("fps"):
            r.fps = int(rs["fps"])
        if rs.get("transparent") is not None:
            r.film_transparent = bool(rs["transparent"])
        draft = rs.get("quality") == "draft"
        r.resolution_percentage = 50 if draft else 100
        samples = rs.get("samples") or (16 if draft else None)
        if samples:
            if r.engine == "CYCLES":
                scene.cycles.samples = int(samples)
            elif hasattr(scene, "eevee"):
                scene.eevee.taa_render_samples = int(samples)
        if r.engine == "CYCLES" and rs.get("gpu", True):
            ab.use_gpu()
        if rs.get("start") is not None:
            scene.frame_start = int(rs["start"])
        if rs.get("end") is not None:
            scene.frame_end = int(rs["end"])
        kind, target = rs["kind"], rs["output"]
        os.makedirs(os.path.dirname(target) if kind != "sequence" else target, exist_ok=True)
        r.use_file_extension = True
        if kind == "image":
            ext = os.path.splitext(target)[1].lower()
            r.image_settings.file_format = "JPEG" if ext in (".jpg", ".jpeg") else "PNG"
            if r.image_settings.file_format == "PNG":
                r.image_settings.color_mode = "RGBA" if r.film_transparent else "RGB"
            r.use_file_extension = False
            r.filepath = target
            scene.frame_set(int(rs.get("frame", scene.frame_start)))
            print("ATTOME_FRAMES 1 1", flush=True)
            bpy.ops.render.render(write_still=True)
            out["render"] = {"path": target, "frames": 1}
        elif kind == "sequence":
            r.image_settings.file_format = "PNG"
            r.image_settings.color_mode = "RGBA" if r.film_transparent else "RGB"
            r.filepath = os.path.join(target, "frame_")
            print("ATTOME_FRAMES %d %d" % (scene.frame_start, scene.frame_end), flush=True)
            bpy.ops.render.render(animation=True)
            out["render"] = {"path": target, "frames": scene.frame_end - scene.frame_start + 1, "pattern": "frame_####.png"}
        else:
            ext = os.path.splitext(target)[1]
            base = os.path.splitext(target)[0]
            if hasattr(r.image_settings, "media_type"):  # Blender 5: a movie is a media type of its own
                r.image_settings.media_type = "VIDEO"
            r.image_settings.file_format = "FFMPEG"
            r.ffmpeg.format = "MPEG4"
            r.ffmpeg.codec = "H264"
            r.ffmpeg.constant_rate_factor = "HIGH"
            r.ffmpeg.audio_codec = "NONE"
            r.filepath = base
            print("ATTOME_FRAMES %d %d" % (scene.frame_start, scene.frame_end), flush=True)
            bpy.ops.render.render(animation=True)
            folder, stem = os.path.dirname(base), os.path.basename(base)
            made = sorted((f for f in os.listdir(folder) if f.startswith(stem) and f.lower().endswith(".mp4")), key=lambda f: os.path.getmtime(os.path.join(folder, f)))
            if made and os.path.join(folder, made[-1]) != target:
                if os.path.exists(target):
                    os.remove(target)
                os.replace(os.path.join(folder, made[-1]), target)
            out["render"] = {"path": target, "frames": scene.frame_end - scene.frame_start + 1}
        out["render"].update(width=r.resolution_x * r.resolution_percentage // 100, height=r.resolution_y * r.resolution_percentage // 100, fps=r.fps,
                             engine=r.engine)
    if spec.get("save"):
        bpy.ops.wm.save_as_mainfile(filepath=spec["save"])
        out["saved"] = spec["save"]
except BaseException:
    out["ok"] = False
    out["error"] = traceback.format_exc()
try:
    json.dumps(out["result"])
except Exception:
    out["result"] = repr(out["result"])
json.dump(out, open(os.path.join(work, "result.json"), "w", encoding="utf8"))
)PY";

const char *kInspect = "result = ab.describe_scene()\n";

#if defined(_WIN32)

// The newest Blender under Program Files\Blender Foundation, if there is one.
fs::path program_files_blender() {
  std::vector<fs::path> found;
  for (const char *env : {"ProgramFiles", "ProgramW6432"})
    if (const char *base = std::getenv(env); base && *base) {
      std::error_code ec;
      const fs::path dir = to_path(base) / "Blender Foundation";
      for (const auto &entry : fs::directory_iterator(dir, ec))
        if (fs::is_regular_file(entry.path() / "blender.exe", ec))
          found.push_back(entry.path() / "blender.exe");
    }
  std::sort(found.begin(), found.end());
  return found.empty() ? fs::path() : found.back();
}

fs::path on_path() {
  const char *path = std::getenv("PATH");
  if (!path)
    return {};
  std::string all = path;
  size_t at = 0;
  while (at <= all.size()) {
    const size_t semi = all.find(';', at);
    const std::string dir = all.substr(at, semi == std::string::npos ? std::string::npos : semi - at);
    std::error_code ec;
    if (!dir.empty() && fs::is_regular_file(to_path(dir) / "blender.exe", ec))
      return to_path(dir) / "blender.exe";
    if (semi == std::string::npos)
      break;
    at = semi + 1;
  }
  return {};
}

std::string clip_lines(const std::vector<std::string> &lines, size_t keep) {
  std::string text;
  const size_t from = lines.size() > keep ? lines.size() - keep : 0;
  for (size_t i = from; i < lines.size(); ++i)
    text += lines[i] + "\n";
  return text;
}

void finish(const std::shared_ptr<Job> &job, Job::State state, std::string detail, const Error *error = nullptr) {
  std::lock_guard lock(job->mutex);
  job->seconds = std::chrono::duration<double>(Clock::now() - job->started).count();
  job->detail = std::move(detail);
  if (error)
    job->error = *error;
  job->state.store(state);
}

fs::path system_tar() {
  wchar_t dir[MAX_PATH] = {};
  const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
  return fs::path(std::wstring(dir, n)) / L"tar.exe";
}

struct Install {
  fs::path root;
  std::shared_ptr<net::Transport> transport;
  fs::path settings_file;
  json settings;
};

void run_install_body(const std::shared_ptr<Job> &job, const Install &in) {
  const auto say = [&](const std::string &text) {
    std::lock_guard lock(job->mutex);
    job->detail = text;
  };
  std::error_code ec;
  fs::create_directories(in.root, ec);
  const fs::path exe = in.root / kFolder / "blender.exe";
  std::string last_line;
  if (!fs::is_regular_file(exe, ec)) {
    models::CatalogFile file;
    file.url = kArchiveUrl;
    file.path = kArchive;
    file.sha256 = kArchiveSha;
    file.size = kArchiveSize;
    models::FetchProgress progress;
    progress.done = &job->units_done;
    progress.fetched = &job->fetched;
    progress.cancel = &job->cancel;
    progress.on_phase = say;
    if (const auto got = models::fetch_file(*in.transport, file, in.root, progress); !got) {
      if (got.error().code == ErrorCode::Cancelled)
        return finish(job, Job::cancelled, "Stopped; start it again to continue");
      const Error e = got.error();
      return finish(job, Job::failed, "", &e);
    }
    say("Unpacking Blender (a minute)");
    const int code = run_process(system_tar(), {L"-xf", (in.root / kArchive).wstring(), L"-C", in.root.wstring()}, job->cancel,
                                 [&](const std::string &line) { last_line = line.substr(0, 300); });
    if (job->cancel.load())
      return finish(job, Job::cancelled, "Stopped while unpacking; start it again");
    if (code != 0 || !fs::is_regular_file(exe, ec)) {
      const Error e{ErrorCode::IoError, "B_UNPACK", "Blender could not be unpacked (tar ended with " + std::to_string(code) + "): " + last_line, {},
                    "Is there room on the drive (about 1.5 GB)?"};
      return finish(job, Job::failed, "", &e);
    }
    fs::remove(in.root / kArchive, ec);
  }
  job->units_done.store(kArchiveSize);
  if (!in.settings_file.empty()) { // this Blender is the one to use from now on
    json all = in.settings;
    all["blender_exe"] = to_utf8(exe);
    all["blender_dir"] = to_utf8(in.root);
    (void)storage::make_dirs(in.settings_file.parent_path());
    (void)storage::atomic_write(in.settings_file, all.dump(2) + "\n");
  }
  {
    std::lock_guard lock(job->mutex);
    job->result = {{"path", to_utf8(exe)}, {"version", kVersion}};
  }
  finish(job, Job::done, "Installed");
}

void run_install(const std::shared_ptr<Job> &job, Install in) {
  prof::set_thread_name("atm-blender");
  try {
    run_install_body(job, in);
  } catch (const std::exception &e) {
    const Error err{ErrorCode::Internal, "B_INTERNAL", std::string("The installation stopped: ") + e.what(), {}, {}};
    finish(job, Job::failed, "", &err);
  }
}

#endif

} // namespace

  // Where Attome puts a Blender it installs: the folder kept in the settings, else the tools folder.
fs::path Engine::Impl::blender_root() const {
  if (const std::string kept = settings().value("blender_dir", std::string()); !kept.empty())
    return to_path(kept);
  return models_dir().parent_path() / "tools" / "blender";
}

  // The Blender to run: ATTOME_BLENDER (the file or its folder), the one Attome installed, one under Program Files, one on the PATH. {} when none.
std::pair<fs::path, std::string> Engine::Impl::blender_exe() const {
#if defined(_WIN32)
  std::error_code ec;
  if (const char *env = std::getenv("ATTOME_BLENDER"); env && *env) {
    if (std::string(env) == "none") // pretend there is no Blender (the tests of the download button)
      return {};
    fs::path p = to_path(env);
    if (fs::is_directory(p, ec))
      p /= "blender.exe";
    if (fs::is_regular_file(p, ec))
      return {p, "ATTOME_BLENDER"};
  }
  if (const std::string kept = settings().value("blender_exe", std::string()); !kept.empty() && fs::is_regular_file(to_path(kept), ec))
    return {to_path(kept), "installed by Attome"};
  if (const fs::path own = blender_root() / kFolder / "blender.exe"; fs::is_regular_file(own, ec))
    return {own, "installed by Attome"};
  if (const fs::path pf = program_files_blender(); !pf.empty())
    return {pf, "found on the computer"};
  if (const fs::path pa = on_path(); !pa.empty())
    return {pa, "found on the PATH"};
#endif
  return {};
}

namespace {
#if defined(_WIN32)
// One run of Blender with a script: the files it needs go in a work folder, its words are collected. Returns result.json's content.
struct Ran {
  json result; // what the runner wrote
  std::vector<std::string> lines;
  int code = 0;
  bool timed_out = false;
  double seconds = 0.0;
};

Result<Ran> run_blender(const fs::path &exe, const fs::path &work_parent, const std::string &helper, const std::string &code, json spec, const std::string &blend,
                        const std::atomic<bool> &cancel, double timeout, const std::function<void(const std::string &)> &on_line) {
  std::error_code ec;
  const fs::path work = work_parent / ("run_" + new_id("blr"));
  fs::create_directories(work, ec);
  const auto cleanup = [&] { fs::remove_all(work, ec); };
  if (ec)
    return fail(ErrorCode::IoError, "B_WORK", "A work folder could not be made in " + to_utf8(work_parent) + ".");
  if (!storage::atomic_write(work / "runner.py", std::string(kRunner)) || !storage::atomic_write(work / "attome_blender.py", helper) ||
      !storage::atomic_write(work / "spec.values", spec.dump())) {
    cleanup();
    return fail(ErrorCode::IoError, "B_WORK", "The script could not be written to " + to_utf8(work) + ".");
  }
  if (!code.empty() && !storage::atomic_write(work / "code.py", code)) {
    cleanup();
    return fail(ErrorCode::IoError, "B_WORK", "The script could not be written to " + to_utf8(work) + ".");
  }
  std::vector<std::wstring> args = {L"-b", L"--factory-startup", L"--python-exit-code", L"1"};
  if (!blend.empty())
    args.push_back(to_path(blend).wstring());
  args.insert(args.end(), {L"--python", (work / "runner.py").wstring(), L"--", work.wstring()});
  std::atomic<bool> stop{false};
  std::atomic<bool> done{false};
  Ran ran;
  const auto began = Clock::now();
  std::thread watchdog([&] { // the caller's stop, or the time allowed
    while (!done.load()) {
      if (cancel.load() || std::chrono::duration<double>(Clock::now() - began).count() > timeout) {
        ran.timed_out = !cancel.load();
        stop.store(true);
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  });
  ran.code = run_process(exe, args, stop, [&](const std::string &line) {
    ran.lines.push_back(line.size() > 400 ? line.substr(0, 400) : line);
    if (on_line)
      on_line(line);
  });
  done.store(true);
  watchdog.join();
  ran.seconds = std::chrono::duration<double>(Clock::now() - began).count();
  if (const auto text = storage::read_file(work / "result.json"))
    ran.result = json::parse(*text, nullptr, false);
  cleanup();
  return ran;
}

std::vector<std::string> readable(const std::vector<std::string> &lines) {
  std::vector<std::string> out;
  for (const std::string &l : lines)
    if (l.rfind("Blender ", 0) != 0 && l.rfind("Read blend", 0) != 0 && l.rfind("Blender quit", 0) != 0 && l.rfind("ATTOME_FRAMES", 0) != 0 && !l.empty())
      out.push_back(l);
  return out;
}

Error blender_failure(const Ran &ran, const std::string &what) {
  std::string message = what;
  std::string hint = "Fix the script and run it again.";
  if (ran.timed_out) {
    message += " It took longer than the time allowed and was stopped.";
    hint = "Give a larger timeout, or make the scene lighter (fewer samples, a smaller size, quality draft).";
  } else if (ran.result.is_object() && ran.result.value("error", json()).is_string()) {
    message += "\n" + ran.result["error"].get<std::string>();
  } else {
    message += " Blender ended with code " + std::to_string(ran.code) + ".\n" + clip_lines(readable(ran.lines), 20);
  }
  if (message.size() > 4000)
    message = message.substr(message.size() - 4000);
  return Error{ErrorCode::InvalidArgument, "B_SCRIPT", message, {}, hint};
}
#endif
} // namespace

  // blender.status {}: is a Blender there, which, how it came, and what blender.install would fetch.
Result<json> Engine::Impl::blender_status(const json &params) {
  const auto [exe, how] = blender_exe();
  json out = {{"installed", !exe.empty()},
              {"download", {{"file", kArchive}, {"version", kVersion}, {"bytes", kArchiveSize}, {"source", "https://download.blender.org/release/Blender5.2/"},
                            {"sha256", kArchiveSha}}},
              {"licence", "Blender is GPL-3.0: a program of its own that Attome runs to draw 3D scenes. Attome does not contain it; blender.install fetches it from the Blender Foundation."},
              {"note", "A Blender that is already on the computer is used as it is (ATTOME_BLENDER names another)."}};
#if defined(_WIN32)
  out["supported"] = true;
  if (!exe.empty()) {
    out["path"] = to_utf8(exe);
    out["source"] = how;
    static std::map<std::string, std::string> seen; // the version of a file does not change: asked once
    const std::string key = to_utf8(exe);
    if (!seen.count(key) && !params.value("quick", false)) { // asking the program takes a second the first time: an interface passes quick
      std::atomic<bool> never{false};
      std::string first;
      run_process(exe, {L"--version"}, never, [&](const std::string &line) {
        if (first.empty() && line.rfind("Blender", 0) == 0)
          first = line;
      });
      seen[key] = first;
    }
    if (seen.count(key))
      out["version"] = seen[key];
  }
#else
  out["supported"] = false;
#endif
  for (const auto &[job_id, job] : jobs)
    if (job->kind == "blender.install" && job->state.load() == Job::running)
      out["job_id"] = job_id;
  return out;
}

  // blender.install {folder?}: a job. A large download, so the caller asks the person first.
Result<json> Engine::Impl::blender_install(const json &params) {
#if !defined(_WIN32)
  (void)params;
  return fail(ErrorCode::Unsupported, "B_UNSUPPORTED", kUnsupported, {}, "Install Blender yourself (blender.org) and set ATTOME_BLENDER to it.");
#else
  for (const auto &[job_id, job] : jobs)
    if (job->kind == "blender.install" && job->state.load() == Job::running)
      return json{{"job_id", job_id}, {"already_running", true}};
  Install in;
  in.root = params.contains("folder") && params["folder"].is_string() && !params.value("folder", std::string()).empty() ? to_path(params.value("folder", std::string()))
                                                                                                                         : blender_root();
  if (!in.root.is_absolute())
    return bad_param("folder", "must be an absolute path");
  in.transport = cfg.transport ? cfg.transport : std::shared_ptr<net::Transport>(net::system_transport());
  in.settings_file = settings_path();
  in.settings = settings();
  auto job = std::make_shared<Job>();
  job->id = new_id("job");
  job->kind = "blender.install";
  job->output = to_utf8(in.root / kFolder);
  job->units_total.store(kArchiveSize);
  jobs[job->id] = job;
  job->thread = std::thread(run_install, job, in);
  return json{{"job_id", job->id}, {"path", to_utf8(in.root / kFolder)}, {"bytes", kArchiveSize}, {"version", kVersion}};
#endif
}

#if defined(_WIN32)
namespace {
std::string helper_source() {
  const std::string_view *data = skills::builtin_file("blender", "scripts/attome_blender.py");
  return data ? std::string(*data) : std::string();
}
} // namespace
#endif

  // blender.run {code | script, blend?, save?, empty?, timeout?}: Python in a headless Blender. `ab` (the helper module) and `bpy` are ready;
  // whatever the code leaves in `result` (JSON) comes back with what it printed. blend: a file to open first; save: true writes the changes back
  // into it, or a path writes a new .blend; empty (default when no blend is given): start from an empty scene, not the default cube.
Result<json> Engine::Impl::blender_run(const json &params) {
#if !defined(_WIN32)
  (void)params;
  return fail(ErrorCode::Unsupported, "B_UNSUPPORTED", kUnsupported, {}, "Install Blender yourself (blender.org) and set ATTOME_BLENDER to it.");
#else
  const fs::path exe = blender_exe().first;
  if (exe.empty())
    return fail(ErrorCode::NotFound, "B_NOT_INSTALLED", "There is no Blender on this computer.", {}, "Ask the person, then install one with blender.install (about 400 MB).");
  std::string code = params.value("code", std::string());
  if (code.empty() && params.contains("script") && params["script"].is_string()) {
    const auto text = storage::read_file(to_path(params["script"].get<std::string>()));
    if (!text)
      return bad_param("script", "is not a file that can be read");
    code = *text;
  }
  if (code.empty())
    return bad_param("code", "is required: Python for Blender (or script: the path of a .py file)");
  ATM_TRY(const BlenderSpec spec, blender_spec(params));
  ATM_CHECK(storage::make_dirs(blender_root() / "work"));
  std::atomic<bool> never{false};
  ATM_TRY(Ran ran, run_blender(exe, blender_root() / "work", helper_source(), code, spec.values, spec.blend, never, params.value("timeout", 300.0), nullptr));
  if (ran.code != 0 || !ran.result.is_object() || !ran.result.value("ok", false))
    return tl::unexpected(blender_failure(ran, "The script did not run."));
  json out = {{"ok", true}, {"result", ran.result.value("result", json())}, {"output", clip_lines(readable(ran.lines), 60)}, {"seconds", ran.seconds}};
  if (ran.result.contains("saved"))
    out["saved"] = ran.result["saved"];
  return out;
#endif
}

  // blender.inspect {blend}: what a .blend holds, as the helper's describe_scene gives it.
Result<json> Engine::Impl::blender_inspect(const json &params) {
#if !defined(_WIN32)
  (void)params;
  return fail(ErrorCode::Unsupported, "B_UNSUPPORTED", kUnsupported, {}, "Install Blender yourself (blender.org) and set ATTOME_BLENDER to it.");
#else
  const fs::path exe = blender_exe().first;
  if (exe.empty())
    return fail(ErrorCode::NotFound, "B_NOT_INSTALLED", "There is no Blender on this computer.", {}, "Ask the person, then install one with blender.install (about 400 MB).");
  ATM_TRY(const std::string *blend, string_param(params, "blend"));
  std::error_code ec;
  if (!fs::is_regular_file(to_path(*blend), ec))
    return bad_param("blend", "is not a file");
  ATM_CHECK(storage::make_dirs(blender_root() / "work"));
  std::atomic<bool> never{false};
  ATM_TRY(Ran ran, run_blender(exe, blender_root() / "work", helper_source(), kInspect, json{{"empty", false}}, *blend, never, params.value("timeout", 120.0), nullptr));
  if (ran.code != 0 || !ran.result.is_object() || !ran.result.value("ok", false))
    return tl::unexpected(blender_failure(ran, "The file could not be read."));
  return ran.result.value("result", json());
#endif
}

  // blender.render {blend? and/or code?, output, ...}: a job that draws the scene. output: a .png / .jpg file (one frame), a .mp4 file (the
  // frame range as a video), or a folder (a PNG sequence, frame_0001.png...; with `transparent` they keep what is empty clear, to lay over a film).
Result<json> Engine::Impl::blender_render(const json &params) {
#if !defined(_WIN32)
  (void)params;
  return fail(ErrorCode::Unsupported, "B_UNSUPPORTED", kUnsupported, {}, "Install Blender yourself (blender.org) and set ATTOME_BLENDER to it.");
#else
  const fs::path exe = blender_exe().first;
  if (exe.empty())
    return fail(ErrorCode::NotFound, "B_NOT_INSTALLED", "There is no Blender on this computer.", {}, "Ask the person, then install one with blender.install (about 400 MB).");
  ATM_TRY(const std::string *output, string_param(params, "output"));
  if (output->empty() || !to_path(*output).is_absolute())
    return bad_param("output", "must be an absolute path: a .png or .jpg file, a .mp4 file, or a folder");
  std::string code = params.value("code", std::string());
  const std::string blend = params.value("blend", std::string());
  if (blend.empty() && code.empty())
    return bad_param("blend", "or code is required: the scene to draw");
  ATM_TRY(BlenderSpec spec, blender_spec(params));
  std::string ext = to_utf8(to_path(*output).extension());
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  const bool image = ext == ".png" || ext == ".jpg" || ext == ".jpeg", video = ext == ".mp4";
  if (!ext.empty() && !image && !video)
    return bad_param("output", "has an extension that is none of .png .jpg .mp4; leave it out for a folder of PNG pictures");
  json render = {{"kind", image ? "image" : video ? "video" : "sequence"}, {"output", *output}};
  for (const char *key : {"engine", "width", "height", "fps", "transparent", "quality", "samples", "gpu", "start", "end", "frame"})
    if (params.contains(key))
      render[key] = params[key];
  spec.values["render"] = std::move(render);
  ATM_CHECK(storage::make_dirs(blender_root() / "work"));
  auto job = std::make_shared<Job>();
  job->id = new_id("job");
  job->kind = "blender.render";
  job->output = *output;
  jobs[job->id] = job;
  const fs::path work_parent = blender_root() / "work";
  const std::string helper = helper_source();
  const double timeout = params.value("timeout", 3600.0);
  const std::string out_path = *output;
  job->thread = std::thread([job, exe, work_parent, helper, code, spec, timeout, out_path] {
    prof::set_thread_name("atm-blender");
    int64_t first = 1;
    const auto on_line = [&](const std::string &line) {
      if (line.rfind("ATTOME_FRAMES", 0) == 0) {
        int a = 1, b = 1;
        if (std::sscanf(line.c_str(), "ATTOME_FRAMES %d %d", &a, &b) == 2) {
          first = a;
          job->units_total.store(std::max(1, b - a + 1));
        }
      } else if (const size_t at = line.find("Fra:"); at != std::string::npos) {
        const int n = std::atoi(line.c_str() + at + 4);
        job->units_done.store(std::max<int64_t>(0, std::min<int64_t>(job->units_total.load(), n - first)));
        std::lock_guard lock(job->mutex);
        job->detail = line.substr(at, 120);
      }
    };
    try {
      const auto ran = run_blender(exe, work_parent, helper, code, spec.values, spec.blend, job->cancel, timeout, on_line);
      if (job->cancel.load())
        return finish(job, Job::cancelled, "Stopped");
      if (!ran) {
        const Error e = ran.error();
        return finish(job, Job::failed, "", &e);
      }
      std::error_code ec;
      const bool made = ran->result.is_object() && ran->result.value("ok", false) && ran->result.contains("render") && ran->result["render"].is_object();
      const bool exists = made && fs::exists(to_path(out_path), ec);
      if (ran->code != 0 || !made || !exists) {
        const Error e = blender_failure(*ran, "The scene was not drawn.");
        return finish(job, Job::failed, "", &e);
      }
      job->units_done.store(job->units_total.load());
      {
        std::lock_guard lock(job->mutex);
        job->result = ran->result["render"];
        job->result["seconds"] = ran->seconds;
      }
      finish(job, Job::done, "Drawn");
    } catch (const std::exception &e) {
      const Error err{ErrorCode::Internal, "B_INTERNAL", std::string("The render stopped: ") + e.what(), {}, {}};
      finish(job, Job::failed, "", &err);
    }
  });
  return json{{"job_id", job->id}, {"output", *output}};
#endif
}

  // The part of blender.run's and blender.render's parameters that both read: the file to open, what to save, whether to start empty.
Result<Engine::Impl::BlenderSpec> Engine::Impl::blender_spec(const json &params) const {
  BlenderSpec s;
  s.blend = params.value("blend", std::string());
  std::error_code ec;
  if (!s.blend.empty() && !fs::is_regular_file(to_path(s.blend), ec))
    return fail(ErrorCode::NotFound, "B_BLEND", "The file \"" + s.blend + "\" was not found.", {}, "Give the path of a .blend file that exists, or leave it out to start empty.");
  s.values = json::object();
  s.values["empty"] = params.value("empty", s.blend.empty());
  if (params.contains("save")) {
    if (params["save"].is_string() && !params["save"].get<std::string>().empty()) {
      s.values["save"] = params["save"];
      std::error_code mk;
      fs::create_directories(to_path(params["save"].get<std::string>()).parent_path(), mk);
    } else if (params["save"].is_boolean() && params["save"].get<bool>()) {
      if (s.blend.empty())
        return bad_param("save", "true needs blend (the file to write back to); give a path to save a new file");
      s.values["save"] = s.blend;
    }
  }
  return s;
}

} // namespace atm::api
