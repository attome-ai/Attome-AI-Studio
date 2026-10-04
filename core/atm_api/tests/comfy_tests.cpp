#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <filesystem>

#include "atm/api/gen_comfy.hpp"
#include "atm/base/id.hpp"
#include "atm/storage/file.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

// A ComfyUI in memory: it takes an upload, queues a prompt, says "not yet" once, then finishes with one video.
struct FakeComfy final : atm::net::Transport {
  std::vector<std::string> urls;
  json graph;
  int history_calls = 0;
  bool refuse_models = false, fail_run = false;
  // What a real ComfyUI says while it runs; then the socket closes.
  atm::Result<void> listen(const std::string &url, const std::function<bool(std::string_view)> &on_text, const std::atomic<bool> *) override {
    listened = url.substr(url.find('/', 8));
    for (const char *message : {R"({"type":"status","data":{"status":{}}})", R"({"type":"executing","data":{"node":"unet"}})",
                                R"({"type":"executing","data":{"node":"sample"}})", R"({"type":"progress","data":{"node":"sample","value":3,"max":8}})",
                                R"({"type":"executing","data":{"node":"dec"}})"})
      on_text(message);
    return {};
  }
  std::string listened;
  atm::Result<atm::net::Response> get(const atm::net::Request &request, const atm::net::OnResponse &on_response, const atm::net::Sink &sink) override {
    urls.push_back(request.url.substr(request.url.find('/', 8)));
    const std::string &path = urls.back();
    atm::net::Response r;
    r.status = 200;
    std::string body = "{}";
    if (path == "/system_stats") {
      body = R"({"system":{"comfyui_version":"0.38.0"},"devices":[{"name":"cuda:0 TEST","vram_total":34190458880}]})";
    } else if (path == "/upload/image") {
      CHECK(request.content_type.rfind("multipart/form-data; boundary=", 0) == 0);
      CHECK(request.body.find("PICTURE") != std::string::npos);
      body = R"({"name":"up.jpg","subfolder":"","type":"input"})";
    } else if (path == "/prompt") {
      graph = json::parse(request.body)["prompt"];
      if (refuse_models) {
        r.status = 400;
        body = R"({"error":{"type":"prompt_outputs_failed_validation"},"node_errors":{"unet":{"errors":[{"type":"value_not_in_list"}]}}})";
      } else {
        body = R"({"prompt_id":"p1"})";
      }
    } else if (path == "/history/p1") {
      if (++history_calls == 1)
        body = "{}";
      else if (fail_run)
        body = R"({"p1":{"status":{"status_str":"error","completed":false,"messages":[["execution_error",{"node_type":"SamplerCustomAdvanced","exception_message":"CUDA out of memory"}]]}}})";
      else
        body = R"({"p1":{"status":{"status_str":"success","completed":true},"outputs":{"save":{"images":[{"filename":"run_00001_.mp4","subfolder":"attome","type":"output"}]}}}})";
    } else if (path.rfind("/view?", 0) == 0) {
      body = "NOT REALLY A VIDEO";
    }
    r.content_length = int64_t(body.size());
    if (!on_response || on_response(r))
      if (sink)
        sink(reinterpret_cast<const uint8_t *>(body.data()), body.size());
    return r;
  }
};

} // namespace

TEST_CASE("comfyui: one generate_video step becomes one ComfyUI graph, and its video comes back", "[gen][comfyui]") {
  const fs::path dir = fs::temp_directory_path() / atm::new_id("attome-comfy");
  fs::create_directories(dir);
  auto server = std::make_shared<FakeComfy>();
  atm::api::ComfyProvider comfy("127.0.0.1:8188/", server);
  comfy.poll_ms = 0;
  const char *h3 = "minimax-h3.fl2va.turbo8-int8";
  CHECK(comfy.offers(h3, "generate_video"));
  CHECK_FALSE(comfy.offers(h3, "sample")); // ComfyUI cannot be opened into the blocks from outside
  CHECK_FALSE(comfy.offers("attome-mock", "generate_video"));
  const json status = comfy.status();
  CHECK(status["reachable"] == true);
  CHECK(status["version"] == "0.38.0");
  CHECK(status["address"] == "http://127.0.0.1:8188");

  REQUIRE(atm::storage::atomic_write(dir / "start.jpg", "PICTURE"));
  atm::gen::StepRequest request;
  request.run_id = "run_1";
  request.kind = "generate_video";
  request.model = h3;
  request.settings = {{"steps", 8}, {"attention", "int8"}};
  request.inputs = {{"prompt", "A robot walks"}, {"seed", 7}, {"seconds", 5}, {"width", 1280}, {"height", 704}, {"start_image", (dir / "start.jpg").string()}};
  request.outputs = {{"video", (dir / "video.mp4").string()}, {"last_frame", (dir / "last_frame.jpg").string()}};
  std::mutex said_mutex;
  std::vector<std::string> said;
  request.progress = [&](std::string_view phase, int at, int of) {
    std::lock_guard lock(said_mutex);
    said.push_back(std::string(phase) + (of > 0 ? " " + std::to_string(at) + "/" + std::to_string(of) : ""));
  };
  REQUIRE(comfy.run(request));
  CHECK(*atm::storage::read_file(dir / "video.mp4") == "NOT REALLY A VIDEO");
  // What was heard on the WebSocket became progress: the node that runs, and the sampler's step.
  CHECK(server->listened == "/ws?clientId=run_1");
  CHECK(std::find(said.begin(), said.end(), "loading the model") != said.end());
  CHECK(std::find(said.begin(), said.end(), "sampling 3/8") != said.end());
  CHECK(std::find(said.begin(), said.end(), "decoding") != said.end());
  request.progress = nullptr;
  CHECK(server->history_calls == 2);
  // The graph: the catalog's file names, the Turbo add-on, INT8 attention, the model's frame grid, the start picture.
  const json &g = server->graph;
  CHECK(g["unet"]["inputs"]["unet_name"] == "minimax_h3_fl2va_pruned_int8_convrot.safetensors");
  CHECK(g["vae"]["inputs"]["vae_name"] == "minimax_h3_video_vae_int8_convrot.safetensors");
  CHECK(g["avae"]["inputs"]["vae_name"] == "minimax_h3_audio_vae_fp32.safetensors");
  CHECK(g["lora"]["inputs"]["lora_name"] == "minimax_h3_fl2v_turbo_8step_v1.0_comfyui_bf16.safetensors");
  CHECK(g["attn"]["class_type"] == "ModelAttentionBackend");
  CHECK(g["sigmas"]["inputs"]["steps"] == 8);
  CHECK(g["sigmas"]["inputs"]["model"] == json::array({"attn", 0}));
  CHECK(g["cond"]["inputs"]["length"] == 124);
  CHECK(g["cond"]["inputs"]["first_frame"] == json::array({"first", 0}));
  CHECK(g["first"]["inputs"]["image"] == "up.jpg");
  CHECK(g["noise"]["inputs"]["noise_seed"] == 7);
  CHECK(server->urls.back() == "/view?filename=run_00001_.mp4&subfolder=attome&type=output");

  request.settings["attention"] = "default";
  request.inputs.erase("start_image");
  server->history_calls = 0;
  REQUIRE(comfy.run(request));
  CHECK_FALSE(server->graph.contains("attn"));
  CHECK_FALSE(server->graph.contains("first"));

  server->refuse_models = true; // ComfyUI cannot see the model files
  auto refused = comfy.run(request);
  REQUIRE_FALSE(refused);
  CHECK(refused.error().rule == "E_MODEL_FILE");
  CHECK(refused.error().hint.find("extra_model_paths") != std::string::npos);
  server->refuse_models = false;
  server->fail_run = true;
  server->history_calls = 0;
  auto failed = comfy.run(request);
  REQUIRE_FALSE(failed);
  CHECK(failed.error().rule == "E_MEMORY");

  std::atomic<bool> cancel{true};
  request.cancel = &cancel;
  auto stopped = comfy.run(request);
  REQUIRE_FALSE(stopped);
  CHECK(stopped.error().rule == "E_CANCELLED");
  CHECK(server->urls.back() == "/interrupt");
  std::error_code ec;
  fs::remove_all(dir, ec);
}
