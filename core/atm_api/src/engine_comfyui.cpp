// ComfyUI as something the engine can put on a computer itself: comfyui.status, comfyui.install, comfyui.start, comfyui.stop.
//
// ComfyUI is another program (GPL-3.0) that Attome asks to generate; Attome never contains it. comfyui.install fetches Comfy-Org's own
// portable release (one pinned version, checked against the SHA-256 that release publishes), unpacks it into Attome's tools folder, adds the
// node packs the engine's graphs use (each at one pinned commit of its publisher's repository), points it at Attome's models folder and
// starts it. It listens on 127.0.0.1 only. Windows with an NVIDIA card in this version.
#include "engine_impl.hpp"

#include <fstream>
#include <thread>

#include "atm/models/models.hpp"
#include "atm/net/http.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace atm::api {

namespace {

constexpr const char *kVersion = "0.38.0";
constexpr const char *kArchive = "ComfyUI_windows_portable_nvidia.7z";
constexpr const char *kArchiveUrl = "https://github.com/Comfy-Org/ComfyUI/releases/download/v0.38.0/ComfyUI_windows_portable_nvidia.7z";
constexpr int64_t kArchiveSize = 1994326521; // as the release lists it (read 2026-10-13)
constexpr const char *kArchiveSha = "8f137eac345707fd7e42bcf8e29377415243011ca15522a86aed6c77331fbd56";
constexpr const char *kFolder = "ComfyUI_windows_portable";

// A node pack the engine's graphs use: where it comes from, what it is for, and which of its nodes the engine asks for.
struct Pack {
  const char *name;    // its folder in custom_nodes
  const char *repo;    // "owner/name" on GitHub; empty: Attome's own, written out from this program
  const char *commit;  // the one revision that is fetched
  const char *licence;
  const char *what;
  std::vector<const char *> nodes;
  std::vector<const char *> python; // packages it needs beyond its own requirements.txt
  std::vector<const char *> alone;  // packages installed without what they ask for (they ask for versions ComfyUI's Python cannot have)
};

const std::vector<Pack> &packs() {
  static const std::vector<Pack> all = {
      {"ComfyUI-KJNodes", "Kijai/ComfyUI-KJNodes", "d7641533d6634eb05f8570c980fce8fc8528dc7b", "GPL-3.0",
       "MiniMax H3 with less memory (the low_memory setting)", {"MiniMaxChunkFeedForward", "MiniMaxLowVRAMAttention"}, {}, {}},
      {"Comfyui_Minimax_h3_latent_Upscaler", "LBH-123-AI/Comfyui_Minimax_h3_latent_Upscaler", "40316cf008b2fd8663263270669eb4da23f89d2c", "MIT",
       "MiniMax H3's final stage (the latent upscaler)", {"MinimaxH3LatentUpscaler3D"}, {}, {}},
      // pydub reads sound through a module Python 3.13 no longer has: audioop-lts brings it back.
      {"omnivoice_comfy", "komikndr/omnivoice_comfy", "70b40f5d88c98951f0ef76373614956ca0ed3af5", "Apache-2.0",
       "OmniVoice: speech with a voice described in words", {"OmniVoiceLoader", "OmniVoiceTTS"}, {"audioop-lts==0.2.2"}, {}},
      // kokoro asks for an old numpy and a misaki that has no build for Python 3.13: it goes in alone, with the versions that work beside it
      // (the set a working ComfyUI 0.38.0 was measured with, 2026-10-13).
      {"attome_kokoro", "", "", "Attome's own",
       "Kokoro: speech with a ready-made voice", {"AttomeKokoroTTS"},
       {"misaki==0.7.4", "loguru==0.7.3", "num2words==0.5.6", "phonemizer==3.4.0", "spacy==3.8.16", "spacy-curated-transformers==0.3.1", "soundfile",
        "https://github.com/explosion/spacy-models/releases/download/en_core_web_sm-3.8.0/en_core_web_sm-3.8.0-py3-none-any.whl"
        "#sha256=1932429db727d4bff3deed6b34cfc05df17794f4a52eeb26cf8928f7c1a0fb85"},
       {"kokoro==0.7.16"}},
  };
  return all;
}

// The nodes of ComfyUI itself that the engine's graphs use (video with MiniMax H3, music with ACE-Step, sound and files).
constexpr const char *kCoreNodes[] = {"MiniMaxH3ImageToVideo", "MiniMaxH3ReferenceToVideo", "MiniMaxH3AddGuide", "LTXVConcatAVLatent", "LTXVSeparateAVLatent",
                                      "BlockSparseAttention", "ModelAttentionBackend", "ModelSamplingAuraFlow", "TextEncodeAceStepAudio1.5",
                                      "EmptyAceStep1.5LatentAudio", "SamplerCustomAdvanced", "BasicGuider", "BasicScheduler", "KSamplerSelect", "RandomNoise",
                                      "CreateVideo", "SaveVideo", "LoadVideo", "GetVideoComponents", "LoadAudio", "SaveAudio", "VAEDecodeAudio",
                                      "TrimAudioDuration", "UNETLoader", "DualCLIPLoader", "CLIPLoader", "VAELoader", "LoraLoaderModelOnly"};

const char *kKokoroNode =
#include "comfy_kokoro_node.inc"
    ;

const char *kUnsupported = "Installing ComfyUI from Attome runs on Windows with an NVIDIA card in this version.";
const char *kOwnHint = "On another system install ComfyUI yourself (github.com/Comfy-Org/ComfyUI), start it and give Attome its address with gen.set_comfyui.";

std::string forward(const fs::path &p) {
  std::string s = to_utf8(p);
  std::replace(s.begin(), s.end(), '\\', '/');
  return s;
}

bool answers(net::Transport &transport, const std::string &address) {
  const auto reply = net::fetch(transport, address + "/system_stats");
  return reply && reply->status == 200;
}

#if defined(_WIN32)
fs::path system_tar() {
  wchar_t dir[MAX_PATH] = {};
  const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
  return fs::path(std::wstring(dir, n)) / L"tar.exe";
}

// The process ComfyUI runs as, when Attome started it and it is still that program: its ID is kept in a file beside it.
DWORD running_pid(const fs::path &portable) {
  std::ifstream in(portable / "attome_comfyui.pid");
  DWORD pid = 0;
  in >> pid;
  if (pid == 0)
    return 0;
  const HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h)
    return 0;
  wchar_t image[1024] = {};
  DWORD size = 1024, exit_code = 0;
  const bool named = QueryFullProcessImageNameW(h, 0, image, &size) != 0;
  const bool alive = GetExitCodeProcess(h, &exit_code) && exit_code == STILL_ACTIVE;
  CloseHandle(h);
  std::error_code ec;
  const bool ours = named && fs::equivalent(fs::path(std::wstring(image, size)), portable / "python_embeded" / "python.exe", ec);
  return alive && ours ? pid : 0;
}

// Starts ComfyUI as its own process (no window; what it prints goes to attome_comfyui.log). It keeps running after this program ends.
Result<void> spawn(const fs::path &portable, int port, const fs::path &models) {
  const fs::path python = portable / "python_embeded" / "python.exe";
  std::error_code ec;
  if (!fs::is_regular_file(python, ec) || !fs::is_regular_file(portable / "ComfyUI" / "main.py", ec))
    return fail(ErrorCode::NotFound, "C_NOT_INSTALLED", "ComfyUI is not installed in " + to_utf8(portable) + ".", {}, "Install it with comfyui.install.");
  SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
  const HANDLE log = CreateFileW((portable / "attome_comfyui.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
  if (log == INVALID_HANDLE_VALUE)
    return fail(ErrorCode::IoError, "C_START", "The log file of ComfyUI could not be written in " + to_utf8(portable) + ".");
  SetEnvironmentVariableW(L"ATTOME_MODELS_DIR", models.c_str()); // the child reads it: Attome's Kokoro node looks there for its weights
  std::wstring command = L"\"" + python.wstring() + L"\" -s ComfyUI\\main.py --windows-standalone-build --listen 127.0.0.1 --port " + std::to_wstring(port) +
                         L" --disable-auto-launch";
  // It inherits the log file and nothing else: with this program's own output pipes it would keep whoever reads them waiting for as long as
  // ComfyUI runs.
  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof si;
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdOutput = log;
  si.StartupInfo.hStdError = log;
  si.StartupInfo.hStdInput = nullptr;
  SIZE_T list_size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &list_size);
  std::vector<char> list(list_size);
  si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(list.data());
  HANDLE only[] = {log};
  const bool listed = InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &list_size) &&
                      UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, only, sizeof only, nullptr, nullptr);
  PROCESS_INFORMATION pi{};
  const BOOL ok = listed && CreateProcessW(python.c_str(), command.data(), nullptr, nullptr, TRUE,
                                           CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | EXTENDED_STARTUPINFO_PRESENT, nullptr, portable.c_str(), &si.StartupInfo, &pi);
  if (listed)
    DeleteProcThreadAttributeList(si.lpAttributeList);
  CloseHandle(log);
  if (!ok)
    return fail(ErrorCode::IoError, "C_START", "ComfyUI could not be started (Windows error " + std::to_string(GetLastError()) + ").");
  std::ofstream(portable / "attome_comfyui.pid") << pi.dwProcessId;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return {};
}

bool stop(const fs::path &portable) {
  const DWORD pid = running_pid(portable);
  if (pid == 0)
    return false;
  const HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
  if (!h)
    return false;
  TerminateProcess(h, 0);
  WaitForSingleObject(h, 5000);
  CloseHandle(h);
  std::error_code ec;
  fs::remove(portable / "attome_comfyui.pid", ec);
  return true;
}
#endif

// Waits until ComfyUI answers (loading its Python and the node packs takes from 15 s to a few minutes the first time).
bool wait_answer(net::Transport &transport, const std::string &address, double seconds, const std::atomic<bool> *cancel) {
  const auto began = Clock::now();
  while (std::chrono::duration<double>(Clock::now() - began).count() < seconds) {
    if (cancel && cancel->load())
      return false;
    if (answers(transport, address))
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  }
  return false;
}

// Which of the nodes the engine uses this ComfyUI has: {core_missing: [...], packs: [{name, ready, missing}]}.
json node_report(net::Transport &transport, const std::string &address) {
  const auto reply = net::fetch(transport, address + "/object_info");
  const json info = reply && reply->status == 200 ? json::parse(reply->body, nullptr, false) : json();
  json out = {{"read", info.is_object()}};
  if (!info.is_object())
    return out;
  json core = json::array();
  for (const char *n : kCoreNodes)
    if (!info.contains(n))
      core.push_back(n);
  out["core_missing"] = std::move(core);
  json list = json::array();
  for (const Pack &p : packs()) {
    json missing = json::array();
    for (const char *n : p.nodes)
      if (!info.contains(n))
        missing.push_back(n);
    list.push_back({{"name", p.name}, {"ready", missing.empty()}, {"missing", std::move(missing)}});
  }
  out["packs"] = std::move(list);
  return out;
}

void finish(const std::shared_ptr<Job> &job, Job::State state, std::string detail, const Error *error = nullptr) {
  std::lock_guard lock(job->mutex);
  job->seconds = std::chrono::duration<double>(Clock::now() - job->started).count();
  job->detail = std::move(detail);
  if (error)
    job->error = *error;
  job->state.store(state);
}

struct Install {
  fs::path root, models;
  int port = 8188;
  std::vector<std::string> wanted; // names of the packs
  std::shared_ptr<net::Transport> transport;
};

#if defined(_WIN32)
void run_install_body(const std::shared_ptr<Job> &job, const Install &in) {
  const auto say = [&](const std::string &text) {
    std::lock_guard lock(job->mutex);
    job->detail = text;
  };
  const auto stopped = [&] { return job->cancel.load(); };
  const fs::path portable = in.root / kFolder, python = portable / "python_embeded" / "python.exe";
  std::error_code ec;
  fs::create_directories(in.root, ec);
  std::string last_line;
  const auto keep = [&](const std::string &line) {
    if (!line.empty())
      last_line = line.size() > 300 ? line.substr(0, 300) : line;
  };

  // 1. ComfyUI itself, unless it is there already.
  if (!fs::is_regular_file(python, ec) || !fs::is_regular_file(portable / "ComfyUI" / "main.py", ec)) {
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
    say("Unpacking ComfyUI (a few minutes)");
    const int code = run_process(system_tar(), {L"-xf", (in.root / kArchive).wstring(), L"-C", in.root.wstring()}, job->cancel, keep);
    if (stopped())
      return finish(job, Job::cancelled, "Stopped while unpacking; start it again");
    if (code != 0 || !fs::is_regular_file(python, ec)) {
      const Error e{ErrorCode::IoError, "C_UNPACK", "ComfyUI could not be unpacked (tar ended with " + std::to_string(code) + "): " + last_line, {},
                    "Is there room on the drive (about 8 GB)? Windows 10 or later is needed to unpack a .7z."};
      return finish(job, Job::failed, "", &e);
    }
    fs::remove(in.root / kArchive, ec); // the archive is not needed any more
  }
  job->units_done.store(kArchiveSize);

  // 2. It reads Attome's models folder: one copy of each model serves both.
  {
    const fs::path yaml = portable / "ComfyUI" / "extra_model_paths.yaml";
    const auto had = storage::read_file(yaml);
    if (!had || had->find("attome:") == std::string::npos) {
      std::string text = had ? *had + "\n" : std::string();
      text += "# The Attome model store: the folder Attome downloads models into. One copy of each model serves both.\nattome:\n    base_path: " + forward(in.models) +
              "\n    is_default: true\n    diffusion_models: diffusion_models/\n    text_encoders: text_encoders/\n    vae: vae/\n    loras: loras/\n"
              "    latent_upscale_models: latent_upscale_models/\n";
      (void)storage::atomic_write(yaml, text);
    }
  }

  // 3. The node packs, each at its pinned commit, and what they need of Python. One that fails is reported; the rest still go in.
  json pack_results = json::array();
  for (const Pack &p : packs()) {
    if (std::find(in.wanted.begin(), in.wanted.end(), std::string(p.name)) == in.wanted.end())
      continue;
    if (stopped())
      return finish(job, Job::cancelled, "Stopped; start it again to continue");
    const fs::path dest = portable / "ComfyUI" / "custom_nodes" / p.name;
    json r = {{"name", p.name}, {"licence", p.licence}};
    std::string problem;
    if (!fs::exists(dest / "__init__.py", ec)) {
      fs::create_directories(dest, ec);
      if (!*p.repo) {
        if (!storage::atomic_write(dest / "__init__.py", std::string(kKokoroNode)))
          problem = "its file could not be written";
      } else {
        say(std::string("Fetching the node pack ") + p.name);
        const fs::path zip = in.root / (std::string(p.name) + ".zip");
        std::ofstream out(zip, std::ios::binary);
        int status = 0;
        const auto got = in.transport->get({std::string("https://github.com/") + p.repo + "/archive/" + p.commit + ".zip", 0, {}, {}},
                                           [&](const net::Response &response) { return (status = response.status) == 200; },
                                           [&](const uint8_t *data, size_t size) { return bool(out.write(reinterpret_cast<const char *>(data), std::streamsize(size))); });
        out.close();
        if (!got || status != 200) {
          problem = "GitHub did not give it (status " + std::to_string(status) + ")";
        } else if (run_process(system_tar(), {L"-xf", zip.wstring(), L"-C", dest.wstring(), L"--strip-components=1"}, job->cancel, keep) != 0 ||
                   !fs::exists(dest / "__init__.py", ec)) {
          problem = "it could not be unpacked: " + last_line;
        }
        fs::remove(zip, ec);
      }
      if (!problem.empty())
        fs::remove_all(dest, ec); // nothing half-made is left for ComfyUI to stumble on
    }
    if (problem.empty() && !p.alone.empty()) {
      std::vector<std::wstring> pip = {L"-s", L"-m", L"pip", L"install", L"--no-warn-script-location", L"--disable-pip-version-check", L"--no-deps"};
      for (const char *package : p.alone)
        pip.push_back(to_path(package).wstring());
      say(std::string("Installing what ") + p.name + " needs of Python");
      last_line.clear();
      if (run_process(python, pip, job->cancel, keep) != 0)
        problem = "its Python packages could not be installed: " + last_line;
    }
    if (problem.empty()) {
      std::vector<std::wstring> pip = {L"-s", L"-m", L"pip", L"install", L"--no-warn-script-location", L"--disable-pip-version-check"};
      const size_t base = pip.size();
      if (fs::is_regular_file(dest / "requirements.txt", ec)) {
        pip.push_back(L"-r");
        pip.push_back((dest / "requirements.txt").wstring());
      }
      for (const char *package : p.python)
        pip.push_back(to_path(package).wstring());
      if (pip.size() > base) {
        say(std::string("Installing what ") + p.name + " needs of Python");
        last_line.clear();
        if (run_process(python, pip, job->cancel, keep) != 0)
          problem = "its Python packages could not be installed: " + last_line;
      }
    }
    r["installed"] = problem.empty();
    if (!problem.empty())
      r["problem"] = problem;
    pack_results.push_back(std::move(r));
  }

  // 4. Start it and see that the nodes the engine uses are there.
  if (stopped())
    return finish(job, Job::cancelled, "Stopped; ComfyUI is installed, start it with comfyui.start");
  const std::string address = "http://127.0.0.1:" + std::to_string(in.port);
  json result = {{"path", to_utf8(portable)}, {"version", kVersion}, {"address", address}, {"packs", std::move(pack_results)}};
  stop(portable); // an earlier run of it does not know the new node packs
  say("Starting ComfyUI");
  if (const auto started = spawn(portable, in.port, in.models); !started) {
    result["running"] = false;
    result["problem"] = started.error().message;
  } else if (!wait_answer(*in.transport, address, 420.0, &job->cancel)) {
    result["running"] = false;
    result["problem"] = "ComfyUI was started but did not answer in 7 minutes; see attome_comfyui.log beside it.";
  } else if (running_pid(portable) == 0) { // something answers there, but the one that was started has ended: it is another program's port
    result["running"] = false;
    result["problem"] = "The ComfyUI that was started ended at once (is the port " + std::to_string(in.port) + " taken?); see attome_comfyui.log beside it.";
  } else {
    result["running"] = true;
    result["nodes"] = node_report(*in.transport, address);
  }
  {
    std::lock_guard lock(job->mutex);
    job->result = std::move(result);
  }
  finish(job, Job::done, "Installed");
}

void run_install(const std::shared_ptr<Job> &job, Install in) {
  prof::set_thread_name("atm-comfyui");
  try {
    run_install_body(job, in);
  } catch (const std::exception &e) {
    const Error err{ErrorCode::Internal, "C_INTERNAL", std::string("The installation stopped: ") + e.what(), {}, {}};
    finish(job, Job::failed, "", &err);
  }
}
#endif

} // namespace

  // Where Attome's own ComfyUI is, or would be put: the folder chosen at install (kept in the settings), else the tools folder.
fs::path Engine::Impl::comfyui_root() const {
  if (const std::string kept = settings().value("comfyui_dir", std::string()); !kept.empty())
    return to_path(kept);
  return models_dir().parent_path() / "tools" / "comfyui";
}

  // comfyui.status {}: is Attome's own ComfyUI installed, is it running, does the ComfyUI the engine talks to answer, and has it the nodes
  // the engine uses. Also what comfyui.install would fetch, with the licences.
Result<json> Engine::Impl::comfyui_status(const json &) {
  const fs::path root = comfyui_root(), portable = root / kFolder;
  std::error_code ec;
  const bool installed = fs::is_regular_file(portable / "python_embeded" / "python.exe", ec) && fs::is_regular_file(portable / "ComfyUI" / "main.py", ec);
  json list = json::array();
  for (const Pack &p : packs())
    list.push_back({{"name", p.name}, {"licence", p.licence}, {"for", p.what}, {"source", *p.repo ? std::string("https://github.com/") + p.repo : std::string("Attome")},
                    {"commit", p.commit}, {"installed", installed && fs::exists(portable / "ComfyUI" / "custom_nodes" / p.name / "__init__.py", ec)}});
  json out = {{"installed", installed}, {"path", to_utf8(portable)}, {"version", kVersion}, {"address", comfyui_address}, {"packs", std::move(list)},
              {"download", {{"file", kArchive}, {"bytes", kArchiveSize}, {"source", "https://github.com/Comfy-Org/ComfyUI/releases/tag/v0.38.0"}}},
              {"licence", "ComfyUI is GPL-3.0: a program of its own that Attome asks to generate. Attome does not contain it; it is fetched from its publisher."},
              {"note", "The models are separate downloads (models.list, models.fetch). MiniMax H3's licence asks for a paid licence for commercial use of what it generates."}};
#if defined(_WIN32)
  out["supported"] = true;
  out["started_by_attome"] = installed && running_pid(portable) != 0;
#else
  out["supported"] = false;
  out["started_by_attome"] = false;
#endif
  const std::shared_ptr<net::Transport> transport = cfg.transport ? cfg.transport : std::shared_ptr<net::Transport>(net::system_transport());
  const bool up = !comfyui_address.empty() && answers(*transport, comfyui_address);
  out["running"] = up;
  if (up)
    out["nodes"] = node_report(*transport, comfyui_address);
  for (const auto &[job_id, job] : jobs)
    if (job->kind == "comfyui.install" && job->state.load() == Job::running)
      out["job_id"] = job_id;
  return out;
}

  // comfyui.install {folder?, port?, packs?}: a job. The address is remembered at once, so that the engine talks to this ComfyUI when it is up.
Result<json> Engine::Impl::comfyui_install(const json &params) {
#if !defined(_WIN32)
  (void)params;
  return fail(ErrorCode::Unsupported, "C_UNSUPPORTED", kUnsupported, {}, kOwnHint);
#else
  for (const auto &[job_id, job] : jobs)
    if (job->kind == "comfyui.install" && job->state.load() == Job::running)
      return json{{"job_id", job_id}, {"already_running", true}};
  Install in;
  in.root = params.contains("folder") && params["folder"].is_string() && !params.value("folder", std::string()).empty() ? to_path(params.value("folder", std::string()))
                                                                                                                         : comfyui_root();
  if (!in.root.is_absolute())
    return bad_param("folder", "must be an absolute path");
  in.models = models_dir();
  const int kept_port = settings().value("comfyui_port", 0); // the port this ComfyUI was given before
  in.port = params.value("port", kept_port > 0 ? kept_port : 8188);
  if (in.port < 1024 || in.port > 65535)
    return bad_param("port", "must be between 1024 and 65535");
  for (const Pack &p : packs())
    in.wanted.push_back(p.name);
  if (params.contains("packs")) {
    if (!params["packs"].is_array())
      return bad_param("packs", "must be a list of node pack names (comfyui.status lists them); an empty list: ComfyUI alone");
    in.wanted.clear();
    for (const json &name : params["packs"]) {
      const std::string wanted = name.is_string() ? name.get<std::string>() : std::string();
      if (std::none_of(packs().begin(), packs().end(), [&](const Pack &p) { return wanted == p.name; }))
        return bad_param("packs", ("has no node pack \"" + wanted + "\" (comfyui.status lists them)").c_str());
      in.wanted.push_back(wanted);
    }
  }
  in.transport = cfg.transport ? cfg.transport : std::shared_ptr<net::Transport>(net::system_transport());
  // Another ComfyUI already answers on that port (the user's own): this one takes the next free port.
  const fs::path portable = in.root / kFolder;
  // (When it is Attome's own that answers on its own port, the port is kept: the job stops and restarts that one.)
  const bool own_there = running_pid(portable) != 0 && in.port == kept_port;
  while (!own_there && in.port < 65535 && answers(*in.transport, "http://127.0.0.1:" + std::to_string(in.port)))
    ++in.port;
  const std::string address = "http://127.0.0.1:" + std::to_string(in.port);
  set_comfyui(address);
  if (const fs::path path = settings_path(); !path.empty()) {
    json all = settings();
    all["comfyui"] = address;
    all["comfyui_dir"] = to_utf8(in.root);
    all["comfyui_port"] = in.port;
    ATM_CHECK(storage::make_dirs(path.parent_path()));
    ATM_CHECK(storage::atomic_write(path, all.dump(2) + "\n"));
  }
  auto job = std::make_shared<Job>();
  job->id = new_id("job");
  job->kind = "comfyui.install";
  job->output = to_utf8(portable);
  job->units_total.store(kArchiveSize);
  jobs[job->id] = job;
  job->thread = std::thread(run_install, job, in);
  return json{{"job_id", job->id}, {"path", to_utf8(portable)}, {"address", address}, {"bytes", kArchiveSize}, {"packs", in.wanted}};
#endif
}

  // comfyui.start {port?}: starts Attome's own ComfyUI and waits until it answers. It keeps running until comfyui.stop.
Result<json> Engine::Impl::comfyui_start(const json &params) {
#if !defined(_WIN32)
  (void)params;
  return fail(ErrorCode::Unsupported, "C_UNSUPPORTED", kUnsupported, {}, kOwnHint);
#else
  const fs::path portable = comfyui_root() / kFolder;
  const std::shared_ptr<net::Transport> transport = cfg.transport ? cfg.transport : std::shared_ptr<net::Transport>(net::system_transport());
  const int port = params.value("port", settings().value("comfyui_port", 8188));
  const std::string address = "http://127.0.0.1:" + std::to_string(port);
  const bool was = running_pid(portable) != 0;
  if (!was) {
    if (answers(*transport, address))
      return fail(ErrorCode::InvalidArgument, "C_PORT", "Another program already answers at " + address + ".", {},
                  "If that is your own ComfyUI, use it: gen.set_comfyui {address}. Otherwise pass another port.");
    ATM_CHECK(spawn(portable, port, models_dir()));
  }
  set_comfyui(address);
  if (const fs::path path = settings_path(); !path.empty()) {
    json all = settings();
    all["comfyui"] = address;
    all["comfyui_port"] = port;
    ATM_CHECK(storage::make_dirs(path.parent_path()));
    ATM_CHECK(storage::atomic_write(path, all.dump(2) + "\n"));
  }
  const double wait = params.value("wait", 300.0);
  if (wait <= 0.0) // an interface that must not stand still: it asks comfyui.status until running is true
    return json{{"running", answers(*transport, address)}, {"starting", true}, {"address", address}, {"already_running", was}};
  if (!wait_answer(*transport, address, wait, nullptr) || running_pid(portable) == 0)
    return fail(ErrorCode::WorkerUnavailable, "C_START", "ComfyUI was started but does not answer yet at " + address + ".", {},
                "It may still be loading: ask comfyui.status in a minute. What it printed is in " + to_utf8(portable / "attome_comfyui.log") + ".");
  return json{{"running", true}, {"address", address}, {"already_running", was}, {"nodes", node_report(*transport, address)}};
#endif
}

  // comfyui.stop {}: stops the ComfyUI Attome started. One the user started is left alone.
Result<json> Engine::Impl::comfyui_stop(const json &) {
#if !defined(_WIN32)
  return fail(ErrorCode::Unsupported, "C_UNSUPPORTED", kUnsupported, {}, kOwnHint);
#else
  return json{{"stopped", stop(comfyui_root() / kFolder)}};
#endif
}

} // namespace atm::api
