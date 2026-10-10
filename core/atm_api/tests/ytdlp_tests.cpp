#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "atm/api/engine.hpp"
#include "atm/base/id.hpp"
#include "atm/net/http.hpp"

namespace fs = std::filesystem;
using atm::api::Engine;
using atm::api::json;

namespace {

// GitHub's release page: a checksum list and a five-byte "program".
struct Release final : atm::net::Transport {
  std::vector<std::string> asked;
  atm::Result<atm::net::Response> get(const atm::net::Request &request, const atm::net::OnResponse &on_response, const atm::net::Sink &sink) override {
    asked.push_back(request.url);
    const bool sums = request.url.size() >= 12 && request.url.compare(request.url.size() - 12, 12, "SHA2-256SUMS") == 0;
    const std::string body = sums ? "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824  yt-dlp.exe\n"
                                    "0000000000000000000000000000000000000000000000000000000000000000  yt-dlp\n"
                                  : "hello";
    atm::net::Response r;
    r.status = 200;
    r.content_length = int64_t(body.size());
    if (on_response && !on_response(r))
      return r;
    sink(reinterpret_cast<const uint8_t *>(body.data()), body.size());
    return r;
  }
};

} // namespace

TEST_CASE("ytdlp: the downloader is optional - not found, installed once from the release (checked by its checksum), and video.fetch refuses bad links", "[ytdlp]") {
  const fs::path root = fs::temp_directory_path() / atm::new_id("attome-ytdlp");
  auto server = std::make_shared<Release>();
  atm::api::EngineConfig cfg;
  cfg.models_dir = (root / "models").string();
  cfg.transport = server;
  Engine engine(cfg);

  const json status = engine.call("ytdlp.status", json::object()).value();
  CHECK(status["tools_dir"] == (root / "tools").string());
  CHECK(status.contains("found"));

  const auto bad = engine.call("video.fetch", {{"url", "file:///c:/x.mp4"}});
  REQUIRE_FALSE(bad);
  CHECK(bad.error().rule == "E_PARAM");
  CHECK_FALSE(engine.call("video.fetch", json::object()));

  const json started = engine.call("ytdlp.install", json::object()).value();
  json state;
  for (int i = 0; i < 400; ++i) {
    state = engine.call("jobs.get", {{"job_id", started["job_id"]}}).value();
    if (state["state"] != "running")
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  INFO(state.dump());
  CHECK(state["state"] == "done");
  CHECK(state["unit"] == "bytes");
  CHECK(fs::file_size(root / "tools" / "yt-dlp.exe") == 5);
  CHECK(engine.call("ytdlp.status", json::object()).value()["found"] == true);
  std::error_code ec;
  fs::remove_all(root, ec);
}
