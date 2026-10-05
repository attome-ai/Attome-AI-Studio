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

namespace {
void make_file(const fs::path &path, int64_t size) { // the right size is all the store looks at
  fs::create_directories(path.parent_path());
  { std::ofstream f(path, std::ios::binary); }
  fs::resize_file(path, uintmax_t(size));
}
} // namespace

TEST_CASE("models.locate: files the user already has count as installed; models.set_folder: downloads go elsewhere", "[models]") {
  const fs::path root = fs::temp_directory_path() / atm::new_id("attome-models-locate");
  const fs::path dir = root / "attome", comfy = root / "ComfyUI_portable";
  auto server = std::make_shared<Refusing>();
  atm::api::EngineConfig cfg;
  cfg.models_dir = dir.string();
  cfg.transport = server;
  Engine engine(cfg);
  const std::string id = "minimax-h3.fl2va.turbo8-int8";
  const int64_t all = 47237843655, audio = 605254808, lora = 1956193000, vae8 = 2811065184;

  // Two of the model's files in a ComfyUI: one where ComfyUI keeps it, one loose in the models folder.
  make_file(comfy / "ComfyUI/models/vae/minimax_h3_audio_vae_fp32.safetensors", audio);
  make_file(comfy / "ComfyUI/models/minimax_h3_fl2v_turbo_8step_v1.0_comfyui_bf16.safetensors", lora);
  make_file(comfy / "ComfyUI/models/vae/minimax_h3_video_vae_fp16.safetensors", 100); // the wrong size: not the file

  const auto nowhere = engine.call("models.locate", {{"folder", (root / "none").string()}, {"id", id}});
  REQUIRE_FALSE(nowhere);
  CHECK(nowhere.error().rule == "M_FOLDER");
  fs::create_directories(root / "empty");
  const json nothing = engine.call("models.locate", {{"folder", (root / "empty").string()}, {"id", id}}).value();
  CHECK(nothing["found"] == 0);
  CHECK(nothing["added"] == false);
  CHECK(engine.call("models.list", json::object()).value()["folders"].empty());

  // The folder a person knows is ComfyUI's own; a subfolder of the models folder works as well, and is the same folder.
  const json found = engine.call("models.locate", {{"folder", comfy.string()}, {"id", id}}).value();
  CHECK(found["found"] == 2);
  CHECK(found["of"] == 6);
  CHECK(found["added"] == true);
  CHECK(found["folder"] == (comfy / "ComfyUI" / "models").string());
  CHECK(found["bytes_missing"] == all - audio - lora);
  const json again = engine.call("models.locate", {{"folder", (comfy / "ComfyUI/models/vae").string()}}).value();
  CHECK(again["found"] == 2);
  CHECK(again["added"] == false);

  json listed = engine.call("models.list", json::object()).value();
  CHECK(listed["folders"].size() == 1);
  CHECK(listed["free_bytes"].get<int64_t>() > 0);
  CHECK(listed["entries"][0]["state"] == "partial");
  CHECK(listed["entries"][0]["bytes"] == audio + lora);
  CHECK(listed["entries"][0]["files"][1]["state"] == "installed");
  CHECK(listed["entries"][0]["files"][3]["state"] == "missing");

  // A download leaves the found files alone and asks for the first one that is missing.
  const json started = engine.call("models.fetch", {{"id", id}}).value();
  CHECK(started["bytes_missing"] == all - audio - lora);
  const json state = wait_for(engine, started["job_id"]);
  CHECK(state["state"] == "failed");
  CHECK(state["units_done"] == audio + lora);
  CHECK(server->calls == 1);
  CHECK_FALSE(fs::exists(dir / "vae/minimax_h3_audio_vae_fp32.safetensors"));

  // Another folder for downloads: what the old one holds stays usable.
  make_file(dir / "vae/minimax_h3_video_vae_int8_convrot.safetensors", vae8);
  const fs::path big = root / "big drive" / "models";
  const json moved = engine.call("models.set_folder", {{"folder", big.string()}}).value();
  CHECK(moved["models_dir"] == big.string());
  CHECK(fs::is_directory(big));
  listed = engine.call("models.list", json::object()).value();
  CHECK(listed["models_dir"] == big.string());
  CHECK(listed["folders"].size() == 2);
  CHECK(listed["entries"][0]["bytes"] == audio + lora + vae8);

  CHECK(engine.call("models.forget_folder", {{"folder", (comfy / "ComfyUI" / "models").string()}}).value()["removed"] == true);
  CHECK(engine.call("models.list", json::object()).value()["entries"][0]["bytes"] == vae8);
  CHECK(engine.call("models.set_folder", {{"folder", ""}}).value()["models_dir"] == dir.string());
  std::error_code ec;
  fs::remove_all(root, ec);
}
