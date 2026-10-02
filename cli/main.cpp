// attome: the command line client. Every command is one Tool call. It goes to the running daemon when there is
// one; otherwise the same Engine runs in this process (one-shot mode) and saves before it exits.

#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>
#include <cstdio>
#include <iostream>
#include <iterator>
#include <thread>

#include <CLI/CLI.hpp>

#include "atm/api/engine.hpp"
#include "atm/api/mcp.hpp"
#include "atm/api/transport.hpp"
#include "atm/base/profiler.hpp"
#include "atm/media/media.hpp"
#include "atm/storage/file.hpp"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

namespace {

using atm::api::json;

struct Options {
  bool as_json = false;
  bool profile = false;
  std::string daemon = "auto"; // auto | never | require
  std::string endpoint = atm::api::default_endpoint();
};

constexpr int kExitDaemonUnavailable = 12;

// The outcome of one Tool call, in the shape `--json` prints.
json call(const Options &opt, const char *tool, json params, bool allow_local = true) {
  if (opt.daemon != "never") {
    if (auto stream = atm::api::connect(opt.endpoint)) {
      const json request = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", tool}, {"params", std::move(params)}};
      std::string body;
      atm::api::FrameReader reader(*stream);
      if (!atm::api::write_frame(*stream, request.dump()) || !reader.read(body))
        return {{"ok", false},
                {"error", {{"code", 0}, {"message", "The daemon closed the connection."}, {"exit", kExitDaemonUnavailable},
                           {"data", {{"hint", "Run the command again."}}}}}};
      json response = json::parse(body, nullptr, false);
      if (response.is_object() && response.contains("result"))
        return {{"ok", true}, {"result", std::move(response["result"])}};
      if (response.is_object() && response.contains("error"))
        return {{"ok", false}, {"error", std::move(response["error"])}};
      return {{"ok", false}, {"error", {{"code", 1900}, {"message", "The daemon sent an unreadable reply."}}}};
    }
  }
  if (opt.daemon == "require" || !allow_local)
    return {{"ok", false},
            {"error", {{"code", 0}, {"message", "No Attome daemon is running."}, {"exit", kExitDaemonUnavailable},
                       {"data", {{"hint", "Start one with: attome daemon start"}}}}}};
  atm::api::Engine engine;
  auto result = engine.call(tool, params);
  engine.save_all(); // one-shot mode: project.json is written before exit
  if (result)
    return {{"ok", true}, {"result", std::move(*result)}};
  return {{"ok", false}, {"error", atm::error_to_json(result.error())}};
}

int finish(const Options &opt, const json &outcome, bool profile_table = false) {
  const bool ok = outcome.value("ok", false);
  if (opt.as_json) {
    std::cout << outcome.dump() << "\n";
  } else if (ok) {
    const json &result = outcome["result"];
    if (profile_table)
      std::cout << atm::prof::format_report(result);
    else if (result.is_object() && result.contains("text"))
      std::cout << result["text"].get<std::string>();
    else
      std::cout << result.dump(2) << "\n";
  } else {
    const json &error = outcome["error"];
    std::cerr << "error: " << error.value("message", "unknown error") << "\n";
    const json data = error.value("data", json::object());
    if (const auto errors = data.find("errors"); errors != data.end() && errors->is_array() && errors->size() > 1)
      for (const json &e : *errors)
        std::cerr << "  - " << e.value("message", "") << "\n      hint: " << e.value("hint", "") << "\n";
    else if (data.contains("hint"))
      std::cerr << "  hint: " << data.value("hint", "") << "\n";
  }
  if (opt.profile) // where this command spent its time (one-shot mode runs the engine in this process)
    std::cerr << atm::prof::format_report(atm::prof::snapshot());
  if (ok)
    return 0;
  const json &error = outcome["error"];
  if (error.contains("exit"))
    return error["exit"].get<int>();
  const int code = error.value("code", 1900);
  return code < 0 ? 2 : atm::exit_code(uint32_t(code)); // negative = JSON-RPC framing or usage errors
}

std::string abs_path(const std::string &path) {
  std::error_code ec;
  const auto abs = std::filesystem::absolute(std::filesystem::path(std::u8string(path.begin(), path.end())), ec);
  const std::u8string s = abs.u8string();
  return ec ? path : std::string(s.begin(), s.end());
}

bool wait_for_daemon(const std::string &endpoint, bool up, int timeout_ms) {
  for (int waited = 0; waited <= timeout_ms; waited += 20) {
    if (bool(atm::api::connect(endpoint)) == up)
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

// `attome mcp --stdio`: one JSON-RPC message per line on stdin and stdout (MCP stdio transport); logs go to stderr.
// Tools run in the daemon, started on demand, so an agent and the editor share one live project. With --daemon never
// they run in this process instead.
int serve_mcp(const Options &opt) {
#if defined(_WIN32)
  _setmode(_fileno(stdin), _O_BINARY); // UTF-8 bytes in and out, and "\n" without "\r"
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  std::unique_ptr<atm::api::Engine> local;
  std::unique_ptr<atm::api::Stream> stream;
  int next_id = 1;
  const auto daemon_call = [&](const std::string &tool, const json &params) -> json {
    for (int attempt = 0; attempt < 2; ++attempt) { // one retry: the daemon may have been restarted in between
      if (!stream && !(stream = atm::api::connect(opt.endpoint))) {
        if (!atm::api::spawn_daemon(opt.endpoint, "") || !wait_for_daemon(opt.endpoint, true, 5000))
          break;
        stream = atm::api::connect(opt.endpoint);
        if (!stream)
          break;
      }
      const json request = {{"jsonrpc", "2.0"}, {"id", next_id++}, {"method", tool}, {"params", params}};
      std::string body;
      atm::api::FrameReader reader(*stream);
      if (atm::api::write_frame(*stream, request.dump()) && reader.read(body)) {
        json response = json::parse(body, nullptr, false);
        if (response.is_object() && response.contains("result"))
          return {{"ok", true}, {"result", std::move(response["result"])}};
        if (response.is_object() && response.contains("error"))
          return {{"ok", false}, {"error", std::move(response["error"])}};
      }
      stream.reset();
    }
    return {{"ok", false},
            {"error", {{"code", 0}, {"message", "The Attome daemon could not be reached."},
                       {"data", {{"hint", "Check that attomed sits next to attome, or run: attome daemon start"}}}}}};
  };
  const auto local_call = [&](const std::string &tool, const json &params) -> json {
    if (!local)
      local = std::make_unique<atm::api::Engine>();
    auto r = local->call(tool, params);
    local->save_all();
    if (r)
      return {{"ok", true}, {"result", std::move(*r)}};
    return {{"ok", false}, {"error", atm::error_to_json(r.error())}};
  };
  atm::api::McpServer server(opt.daemon == "never" ? atm::api::ToolCaller(local_call)
                                                   : atm::api::ToolCaller(daemon_call));
  std::fprintf(stderr, "attome %s: MCP server on stdio (%s)\n", atm::api::kEngineVersion,
               opt.daemon == "never" ? "in-process engine" : opt.endpoint.c_str());
  std::string line;
  while (std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty())
      continue;
    const json message = json::parse(line, nullptr, false);
    const json response = message.is_discarded()
                              ? json{{"jsonrpc", "2.0"}, {"id", nullptr},
                                     {"error", {{"code", -32700}, {"message", "Parse error."}}}}
                              : server.handle(message);
    if (response.is_null())
      continue;
    std::cout << response.dump(-1, ' ', false, json::error_handler_t::replace) << '\n';
    std::cout.flush();
  }
  return 0;
}

} // namespace

int main(int argc, char **argv) {
#if defined(_WIN32)
  SetConsoleOutputCP(CP_UTF8);
#endif
  CLI::App app{"attome - the Attome command line. Every command is also a Tool of the daemon and the MCP server."};
  argv = app.ensure_utf8(argv);
  app.require_subcommand(1);
  app.set_version_flag("--version", std::string("attome ") + atm::api::kEngineVersion);

  Options opt;
  app.add_flag("--json", opt.as_json, "Print exactly one JSON document: {\"ok\":true,\"result\":…} or {\"ok\":false,\"error\":…}");
  app.add_option("--daemon", opt.daemon, "auto: use a running daemon, else run in-process | never | require")
      ->check(CLI::IsMember({"auto", "never", "require"}));
  app.add_option("--endpoint", opt.endpoint, "Daemon pipe name or socket path");
  app.add_flag("--profile", opt.profile, "Print this command's zone profile to stderr");

  std::string project, name, rate = "30", canvas = "1920x1080", level = "summary", id, file, task, label, value;
  int steps = 1, limit = 50, interval = 0;
  bool dry_run = false, reset = false, watch = false, off = false, on = false, no_fsync = false;
  int exit_status = 0;
  atm::prof::set_thread_name("atm-main");

  auto *cmd_new = app.add_subcommand("new", "Create a project: attome new Demo.attome --rate 30000/1001");
  cmd_new->add_option("path", project, "Folder to create (\".attome\" is added when missing)")->required();
  cmd_new->add_option("--name", name, "Project name");
  cmd_new->add_option("--rate", rate, "Frame rate, e.g. 30 or 30000/1001");
  cmd_new->add_option("--canvas", canvas, "Canvas size, e.g. 1920x1080");
  cmd_new->callback([&] {
    json params = {{"path", abs_path(project)}, {"rate", rate}};
    if (!name.empty())
      params["name"] = name;
    int w = 0, h = 0;
    if (std::sscanf(canvas.c_str(), "%dx%d", &w, &h) == 2)
      params["canvas"] = {{"width", w}, {"height", h}};
    exit_status = finish(opt, call(opt, "project.create", std::move(params)));
  });

  auto *cmd_inspect = app.add_subcommand("inspect", "Summary of a project");
  cmd_inspect->add_option("project", project)->required();
  cmd_inspect->add_option("--level", level, "summary | tracks");
  cmd_inspect->callback([&] {
    exit_status = finish(opt, call(opt, "project.inspect", {{"project", abs_path(project)}, {"level", level}}));
  });

  auto *cmd_get = app.add_subcommand("get", "Print one object by Stable ID");
  cmd_get->add_option("project", project)->required();
  cmd_get->add_option("id", id)->required();
  cmd_get->callback(
      [&] { exit_status = finish(opt, call(opt, "project.get", {{"project", abs_path(project)}, {"id", id}})); });

  auto *cmd_patch = app.add_subcommand("patch", "Apply a Patch file (\"-\" reads stdin)");
  cmd_patch->add_option("project", project)->required();
  cmd_patch->add_option("patch", file, "JSON file: {\"ops\": [...]} or a bare array of ops")->required();
  cmd_patch->add_flag("--dry-run", dry_run, "Preview: validate and return the normalized patch, change nothing");
  cmd_patch->add_option("--task", task, "Task ID that groups this edit with others");
  cmd_patch->add_option("--label", label, "Short description shown in the history");
  cmd_patch->callback([&] {
    std::string text;
    if (file == "-") {
      text.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
    } else {
      auto read = atm::storage::read_file(std::filesystem::path(std::u8string(file.begin(), file.end())));
      if (!read) {
        exit_status = finish(opt, {{"ok", false}, {"error", atm::error_to_json(read.error())}});
        return;
      }
      text = std::move(*read);
    }
    json patch = json::parse(text, nullptr, false);
    if (patch.is_discarded()) {
      exit_status = finish(opt, {{"ok", false},
                                 {"error", {{"code", -32700}, {"message", "The patch file is not valid JSON."}}}});
      return;
    }
    if (patch.is_array())
      patch = {{"ops", std::move(patch)}};
    if (!label.empty() && patch.is_object())
      patch["label"] = label;
    json params = {{"project", abs_path(project)}, {"patch", std::move(patch)}, {"dry_run", dry_run}};
    if (!task.empty())
      params["task_id"] = task;
    exit_status = finish(opt, call(opt, "project.patch", std::move(params)));
  });

  const auto simple = [&](const char *command, const char *tool, const char *help) {
    auto *cmd = app.add_subcommand(command, help);
    cmd->add_option("project", project)->required();
    cmd->callback([&, tool] { exit_status = finish(opt, call(opt, tool, {{"project", abs_path(project)}})); });
    return cmd;
  };
  simple("validate", "project.validate", "Check a project against the rules");
  simple("save", "project.save", "Write project.json now");

  for (const bool undo : {true, false}) {
    auto *cmd = app.add_subcommand(undo ? "undo" : "redo", undo ? "Undo the last edit" : "Redo the last undone edit");
    cmd->add_option("project", project)->required();
    cmd->add_option("--steps", steps, "How many edits");
    cmd->callback([&, undo] {
      exit_status = finish(opt, call(opt, undo ? "project.undo" : "project.redo",
                                     {{"project", abs_path(project)}, {"steps", steps}}));
    });
  }

  auto *cmd_history = app.add_subcommand("history", "List the edits of a project");
  cmd_history->add_option("project", project)->required();
  cmd_history->add_option("--limit", limit, "Newest N edits");
  cmd_history->callback([&] {
    exit_status = finish(opt, call(opt, "history.list", {{"project", abs_path(project)}, {"limit", limit}}));
  });

  auto *cmd_time = app.add_subcommand("time", "Convert a time: attome time \"00:00:12;15\" --rate 30000/1001");
  cmd_time->add_option("value", value)->required();
  auto *time_rate = cmd_time->add_option("--rate", rate, "Frame rate for timecode");
  cmd_time->callback([&] {
    json params = {{"value", value}};
    if (time_rate->count() > 0)
      params["rate"] = rate;
    exit_status = finish(opt, call(opt, "time.parse", std::move(params)));
  });

  std::string output;
  int height = 0;
  double seconds = 5.0;

  auto *cmd_probe = app.add_subcommand("probe", "Size, frame rate and duration of a media file");
  cmd_probe->add_option("file", file)->required();
  cmd_probe->callback([&] { exit_status = finish(opt, call(opt, "media.probe", {{"path", abs_path(file)}})); });

  auto *cmd_render = app.add_subcommand("render", "Export a project: attome render Demo.attome -o out.mp4");
  cmd_render->add_option("project", project)->required();
  cmd_render->add_option("-o,--output", output, "The .mp4 file to write")->required();
  cmd_render->add_option("--height", height, "Output height in pixels (default: the canvas height)");
  cmd_render->callback([&] {
    json params = {{"project", abs_path(project)}, {"output", abs_path(output)}};
    if (height > 0)
      params["height"] = height;
    // The job must outlive the call that starts it, so without a daemon one Engine serves the whole command.
    const bool remote = opt.daemon != "never" && bool(atm::api::connect(opt.endpoint));
    std::unique_ptr<atm::api::Engine> local = remote ? nullptr : std::make_unique<atm::api::Engine>();
    const auto invoke = [&](const char *tool, const json &p) -> json {
      if (remote)
        return call(opt, tool, p, false);
      auto r = local->call(tool, p);
      if (r)
        return {{"ok", true}, {"result", std::move(*r)}};
      return {{"ok", false}, {"error", atm::error_to_json(r.error())}};
    };
    json outcome = invoke("render.sequence", params);
    if (outcome.value("ok", false)) {
      const json job = {{"job_id", outcome["result"]["job_id"]}};
      for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        outcome = invoke("jobs.get", job);
        if (!outcome.value("ok", false))
          break;
        const json &state = outcome["result"];
        if (!opt.as_json)
          std::fprintf(stderr, "\r%3.0f%%  %.0f frames per second   ", state.value("progress", 0.0) * 100.0,
                       state.value("fps", 0.0));
        if (state.value("state", "") == "running")
          continue;
        if (!opt.as_json)
          std::fputc('\n', stderr);
        if (state.value("state", "") != "done") { // exit 13: the job failed (MODULES §M16)
          json error = state.value("error", json{{"code", 1600}, {"message", "The export was cancelled."}});
          error["exit"] = 13;
          outcome = {{"ok", false}, {"error", std::move(error)}};
        }
        break;
      }
    }
    exit_status = finish(opt, outcome);
  });

  auto *cmd_sample = app.add_subcommand("sample", "Write a synthetic test clip (moving bar, beeps): attome sample a.mp4");
  cmd_sample->add_option("file", file)->required();
  cmd_sample->add_option("--seconds", seconds, "Length of the clip");
  cmd_sample->add_option("--height", height, "Picture height (16:9), default 720");
  cmd_sample->callback([&] {
    const int h = height > 0 ? height : 720, w = h * 16 / 9;
    const int fps = 30, frames = std::max(1, int(seconds * fps));
    auto fail_with = [&](const atm::Error &e) {
      exit_status = finish(opt, {{"ok", false}, {"error", atm::error_to_json(e)}});
    };
    auto encoder = atm::media::Encoder::create({abs_path(file), w, h, fps, 1, w * h * 4, true});
    if (!encoder)
      return fail_with(encoder.error());
    // The colour comes from the file name, so two samples are easy to tell apart.
    const unsigned tint = unsigned(std::hash<std::string>{}(file));
    std::vector<uint8_t> picture(size_t(w) * size_t(h) * 4), nv12(atm::media::nv12_size(w, h));
    std::vector<float> audio(size_t(atm::media::kAudioRate / fps) * 2);
    const double tone = 330.0 + double(tint % 5) * 110.0;
    size_t sample_index = 0;
    for (int f = 0; f < frames; ++f) {
      const int bar = int(int64_t(f) * w / frames);
      for (int y = 0; y < h; ++y) {
        uint8_t *row = picture.data() + size_t(y) * size_t(w) * 4;
        for (int x = 0; x < w; ++x) {
          const bool on_bar = x >= bar && x < bar + w / 40;
          row[x * 4 + 0] = on_bar ? 255 : uint8_t(40 + (tint & 127) * y / h);
          row[x * 4 + 1] = on_bar ? 255 : uint8_t(40 + ((tint >> 7) & 127) * x / w);
          row[x * 4 + 2] = on_bar ? 255 : uint8_t(40 + ((tint >> 14) & 127));
          row[x * 4 + 3] = 255;
        }
      }
      for (size_t i = 0; i < audio.size() / 2; ++i, ++sample_index) { // a short beep at every full second
        const double t = double(sample_index) / atm::media::kAudioRate;
        const double in_second = t - std::floor(t);
        const float v = in_second < 0.12 ? float(0.3 * std::sin(t * tone * 6.283185307179586)) : 0.0f;
        audio[i * 2] = audio[i * 2 + 1] = v;
      }
      atm::media::bgrx_to_nv12(picture.data(), w, h, nv12.data());
      auto r = (*encoder)->video(nv12.data(), f);
      if (r)
        r = (*encoder)->audio(audio.data(), audio.size() / 2);
      if (!r)
        return fail_with(r.error());
    }
    if (auto r = (*encoder)->finish(); !r)
      return fail_with(r.error());
    exit_status = finish(opt, {{"ok", true}, {"result", {{"path", abs_path(file)}, {"frames", frames}, {"width", w}, {"height", h}}}});
  });

  app.add_subcommand("tools", "List the Tools")->callback([&] {
    exit_status = finish(opt, call(opt, "tools.list", json::object()));
  });

  auto *cmd_profile = app.add_subcommand("profile", "Zone timings of the running daemon, slowest first");
  cmd_profile->add_flag("--reset", reset, "Zero the statistics after reading them");
  cmd_profile->add_flag("--watch", watch, "Refresh every second until interrupted");
  cmd_profile->add_flag("--off", off, "Switch the profiler off");
  cmd_profile->add_flag("--on", on, "Switch the profiler on");
  cmd_profile->callback([&] {
    if (on || off) {
      exit_status = finish(opt, call(opt, "profile.set", {{"enabled", on}}, false));
      return;
    }
    do {
      const json outcome = call(opt, "profile.get", {{"reset", reset}}, false);
      if (watch && !opt.as_json && outcome.value("ok", false))
        std::cout << "\x1b[2J\x1b[H"; // clear the terminal
      exit_status = finish(opt, outcome, true);
      std::cout.flush();
      if (watch)
        std::this_thread::sleep_for(std::chrono::seconds(1));
    } while (watch && exit_status == 0);
  });

  auto *cmd_mcp = app.add_subcommand("mcp", "Serve the Tools to an AI agent over MCP: attome mcp --stdio");
  bool mcp_stdio = false;
  cmd_mcp->add_flag("--stdio", mcp_stdio, "Speak MCP on stdin/stdout (the only transport so far)");
  cmd_mcp->callback([&] { exit_status = serve_mcp(opt); });

  auto *cmd_daemon = app.add_subcommand("daemon", "Start, stop or query the daemon");
  cmd_daemon->require_subcommand(1);
  auto *daemon_start = cmd_daemon->add_subcommand("start", "Start the daemon in the background");
  daemon_start->add_option("--profile-interval", interval, "Seconds between profile reports on the daemon's stderr");
  daemon_start->add_flag("--no-fsync", no_fsync, "Faster edits that are not crash-safe");
  daemon_start->callback([&] {
    if (!atm::api::connect(opt.endpoint)) {
      std::string extra = no_fsync ? "--no-fsync" : "";
      if (interval > 0)
        extra += " --profile-interval " + std::to_string(interval);
      if (!atm::api::spawn_daemon(opt.endpoint, extra) || !wait_for_daemon(opt.endpoint, true, 3000)) {
        exit_status = finish(opt, {{"ok", false},
                                   {"error", {{"code", 0}, {"message", "The daemon did not start."},
                                              {"exit", kExitDaemonUnavailable},
                                              {"data", {{"hint", "Run attomed in a terminal to see why."}}}}}});
        return;
      }
    }
    exit_status = finish(opt, call(opt, "daemon.status", json::object(), false));
  });
  cmd_daemon->add_subcommand("stop", "Save everything and stop the daemon")->callback([&] {
    const json outcome = call(opt, "daemon.shutdown", json::object(), false);
    if (outcome.value("ok", false))
      wait_for_daemon(opt.endpoint, false, 5000); // return only when the projects are saved and unlocked
    exit_status = finish(opt, outcome);
  });
  cmd_daemon->add_subcommand("status", "Show the daemon's open projects")->callback([&] {
    exit_status = finish(opt, call(opt, "daemon.status", json::object(), false));
  });

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError &e) {
    return app.exit(e) == 0 ? 0 : 2; // usage errors exit 2 (MODULES §M16)
  }
  return exit_status;
}
