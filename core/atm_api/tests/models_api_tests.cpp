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

// A server that refuses everything with one status; counts the calls.
struct Refusing final : atm::net::Transport {
  int status = 404;
  int calls = 0;
  atm::Result<atm::net::Response> get(const atm::net::Request &, const atm::net::OnResponse &on_response, const atm::net::Sink &) override {
    ++calls;
    atm::net::Response r;
    r.status = status;
    on_response(r);
    return r;
  }
};

json wait_for(Engine &engine, const std::string &job_id) {
  for (int i = 0; i < 400; ++i) {
    json state = engine.call("jobs.get", {{"job_id", job_id}}).value();
    if (state["state"] != "running")
      return state;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return json{{"state", "timeout"}};
}

} // namespace

TEST_CASE("models.list: the catalog with what is on disk; models.fetch: a job in bytes that reports why it failed", "[models]") {
  const fs::path dir = fs::temp_directory_path() / atm::new_id("attome-models-api");
  auto server = std::make_shared<Refusing>();
  atm::api::EngineConfig cfg;
  cfg.models_dir = dir.string();
  cfg.transport = server;
  Engine engine(cfg);

  const json listed = engine.call("models.list", json::object()).value();
  CHECK(listed["models_dir"] == dir.string());
  REQUIRE(listed["entries"].size() >= 1);
  const json &h3 = listed["entries"][0];
  CHECK(h3["id"] == "minimax-h3.fl2va.turbo8-int8");
  CHECK(h3["state"] == "missing");
  CHECK(h3["bytes"] == 0);
  CHECK(h3["size"] == 47237843655);
  CHECK(h3["files"].size() == 6);
  CHECK_FALSE(h3["licence"].get<std::string>().empty());

  const auto unknown = engine.call("models.fetch", {{"id", "nope"}});
  REQUIRE_FALSE(unknown);
  CHECK(unknown.error().rule == "M_UNKNOWN_MODEL");
  CHECK(unknown.error().hint.find("minimax-h3.fl2va.turbo8-int8") != std::string::npos);

  // A file already in place is recognised by its size, and counted.
  fs::create_directories(dir / "vae");
  {
    std::ofstream f(dir / "vae/minimax_h3_audio_vae_fp32.safetensors", std::ios::binary);
    f.seekp(605254808 - 1);
    f.put('\0');
  }
  const json again = engine.call("models.list", json::object()).value()["entries"][0];
  CHECK(again["state"] == "partial");
  CHECK(again["bytes"] == 605254808);
  CHECK(again["files"][0]["state"] == "installed");
  CHECK(again["files"][1]["state"] == "missing");

  const json started = engine.call("models.fetch", {{"id", "minimax-h3.fl2va.turbo8-int8"}}).value();
  CHECK(started["bytes_total"] == 47237843655);
  CHECK(started["bytes_missing"] == 47237843655 - 605254808);
  const json state = wait_for(engine, started["job_id"]);
  CHECK(state["state"] == "failed");
  CHECK(state["kind"] == "models.fetch");
  CHECK(state["unit"] == "bytes");
  CHECK(state["units_total"] == 47237843655);
  CHECK(state["units_done"] == 605254808); // the installed file counts; nothing else arrived
  CHECK(state["error"]["data"]["rule"] == "M_HTTP_STATUS");
  CHECK(server->calls == 1); // the first missing file was refused with 404: not retried, and the later files not tried
  std::error_code ec;
  fs::remove_all(dir, ec);
}
