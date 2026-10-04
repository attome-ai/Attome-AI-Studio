// attomed: the Attome Daemon. Owns open projects and serves Tools over JSON-RPC.
//
//   attomed                         listen on the per-user endpoint
//   attomed --stdio                 serve one client on stdin/stdout (tests, or a parent that owns the daemon)
//   attomed --profile-interval 10   print the zone profile to stderr every 10 s while there is work

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <thread>

#include <CLI/CLI.hpp>

#include "atm/api/server.hpp"
#include "atm/base/profiler.hpp"

namespace {

std::atomic<bool> g_interrupted{false};

void on_signal(int) { g_interrupted.store(true); }

// Runs `fn` on the writer thread and waits for it.
template <class F> void on_writer(atm::api::Server &server, F &&fn) {
  std::promise<void> done;
  server.post([&] {
    fn();
    done.set_value();
  });
  done.get_future().wait();
}

uint64_t total_frames(const nlohmann::json &snapshot) {
  uint64_t frames = 0;
  for (const auto &t : snapshot["threads"])
    frames += t.value("frames", uint64_t(0));
  return frames;
}

} // namespace

int main(int argc, char **argv) {
  CLI::App app{"attomed - the Attome Daemon"};
  argv = app.ensure_utf8(argv);
  bool stdio = false, no_fsync = false, no_profile = false;
  int profile_interval = 0;
  std::string endpoint = atm::api::default_endpoint();
  app.add_flag("--stdio", stdio, "Serve one client on stdin/stdout instead of the per-user endpoint");
  app.add_option("--endpoint", endpoint, "Pipe name or socket path to listen on");
  app.add_flag("--no-fsync", no_fsync, "Do not wait for the journal to reach the disk (faster, not crash-safe)");
  app.add_flag("--no-profile", no_profile, "Start with the zone profiler switched off");
  app.add_option("--profile-interval", profile_interval, "Print the zone profile to stderr every N seconds");
  app.set_version_flag("--version", std::string("attomed ") + atm::api::kEngineVersion);
  CLI11_PARSE(app, argc, argv);

  atm::prof::set_thread_name("atm-main");
  if (const char *env = std::getenv("ATTOME_PROFILE"); no_profile || (env && env[0] == '0'))
    atm::prof::set_enabled(false);

  atm::api::Engine engine({.fsync = !no_fsync, .user_settings = true});
  atm::api::Server server(engine);

  if (stdio) {
    const auto stream = atm::api::stdio_stream();
    server.serve_stream(*stream);
    on_writer(server, [&] { engine.save_all(); });
    return 0;
  }

  auto listener = atm::api::Listener::listen(endpoint);
  if (!listener) {
    std::fprintf(stderr, "attomed: %s\n", listener.error().message.c_str());
    return listener.error().rule == "R_DAEMON_RUNNING" ? 0 : 6;
  }
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  std::thread([&] {
    atm::prof::set_thread_name("atm-accept");
    server.serve(*listener);
  }).detach();
  std::fprintf(stderr, "attomed %s listening on %s\n", atm::api::kEngineVersion, endpoint.c_str());

  auto last_report = std::chrono::steady_clock::now();
  uint64_t reported_frames = 0;
  while (!server.wait_shutdown(std::chrono::milliseconds(500)) && !g_interrupted.load()) {
    // project.json follows the journal about a second after the last edit (debounced save).
    server.post([&] { engine.save_all(std::chrono::seconds(1)); });
    const auto now = std::chrono::steady_clock::now();
    if (profile_interval > 0 && now - last_report >= std::chrono::seconds(profile_interval)) {
      last_report = now;
      const nlohmann::json snapshot = atm::prof::snapshot();
      if (const uint64_t frames = total_frames(snapshot); frames != reported_frames) { // stay quiet when idle
        reported_frames = frames;
        std::fputs(atm::prof::format_report(snapshot).c_str(), stderr);
      }
    }
  }

  on_writer(server, [&] { engine.save_all(); });
  std::fflush(stderr);
  // Connection threads may still be blocked on their clients; the projects are saved, so leave at once.
  std::_Exit(0);
}
