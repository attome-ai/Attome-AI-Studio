// asr.transcribe through the Engine, with tests/tools/fake_whisper standing in for attome-whisper (see core/atm_asr/tests/asr_tests.cpp).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

#include "atm/api/audio_tools.hpp"
#include "atm/api/engine.hpp"
#include "atm/base/id.hpp"

#if defined(_WIN32)
using atm::api::Engine;
using atm::api::json;
namespace fs = std::filesystem;
namespace audio = atm::api::audio;

namespace {

// The environment the Tool reads, set for one test and put back.
struct Env {
  std::vector<std::pair<std::string, std::string>> was;
  void set(const std::string &name, const std::string &value) {
    const char *old = std::getenv(name.c_str());
    was.emplace_back(name, old ? old : "\x01");
    _putenv_s(name.c_str(), value.c_str());
  }
  ~Env() {
    for (const auto &[name, value] : was)
      _putenv_s(name.c_str(), value == "\x01" ? "" : value.c_str());
  }
};

struct AsrFixture {
  fs::path dir = fs::temp_directory_path() / atm::new_id("attome-asr");
  Env env;
  std::string project = (dir / "A.attome").string();
  std::string tone; // 5 s of sound
  std::unique_ptr<Engine> engine;

  explicit AsrFixture(const std::string &model_text = "ok") {
    fs::create_directories(dir / "models");
    env.set("ATTOME_WHISPER_EXE", ATM_FAKE_WHISPER);
    env.set("ATTOME_MODELS_DIR", (dir / "models").string());
    set_model(model_text);
    engine = std::make_unique<Engine>(atm::api::EngineConfig{.fsync = false});
    REQUIRE(engine->call("project.create", {{"path", project}}));
    std::vector<float> pcm(size_t(5) * 48000 * 2);
    for (size_t i = 0; i < pcm.size() / 2; ++i)
      pcm[i * 2] = pcm[i * 2 + 1] = 0.3f * std::sin(float(i) * 0.05f);
    tone = (dir / "tone.wav").string();
    REQUIRE(audio::write_wav(tone, pcm));
  }
  ~AsrFixture() {
    engine.reset();
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  void set_model(const std::string &text) {
    const std::string path = (dir / "model.bin").string();
    std::ofstream(path, std::ios::binary) << text;
    env.set("ATTOME_WHISPER_MODEL", path);
  }
  std::string add_clip(json extra = json::object()) {
    json op = {{"op", "add_clip"}, {"id", "$new:c"}, {"path", tone}, {"at", "0s"}};
    for (auto it = extra.begin(); it != extra.end(); ++it)
      op[it.key()] = it.value();
    const auto r = engine->call("timeline.edit", {{"project", project}, {"ops", json::array({op})}});
    INFO((r ? "" : r.error().message));
    REQUIRE(r);
    return (*r)["id_map"]["$new:c"];
  }
  // Starts the job and waits for it to leave "running".
  json run(json params) {
    const auto started = engine->call("asr.transcribe", std::move(params));
    INFO((started ? "" : started.error().message + " | " + started.error().hint));
    REQUIRE(started);
    return wait((*started)["job_id"]);
  }
  json wait(const std::string &job) {
    for (int i = 0; i < 1500; ++i) {
      const json state = *engine->call("jobs.get", {{"job_id", job}});
      if (state["state"] != "running")
        return state;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    FAIL("the job did not finish");
    return {};
  }
  std::string refused(json params) {
    const auto r = engine->call("asr.transcribe", std::move(params));
    REQUIRE_FALSE(r);
    return r.error().rule;
  }
};

} // namespace

TEST_CASE("asr.transcribe: a clip is heard and its words come back in the clip's own time", "[asr][engine]") {
  AsrFixture f;
  const std::string clip = f.add_clip();
  const json done = f.run({{"project", f.project}, {"clip", clip}});
  REQUIRE(done["state"] == "done");
  CHECK(done["progress"].get<double>() == 1.0);
  const json result = done["result"];
  CHECK(result["clip"] == clip);
  CHECK(result["language"] == "en");
  REQUIRE(result["words"].size() == 5); // the fake says one word for each second it is given
  CHECK(result["words"][0]["text"] == "w0");
  CHECK(result["words"][2]["start"].get<double>() == Catch::Approx(2.0));
  CHECK(result["words"][2]["end"].get<double>() == Catch::Approx(2.8));
  CHECK(done["detail"] == "5 words");
}

TEST_CASE("asr.transcribe: the part a clip plays, at its speed, gives words in film time", "[asr][engine]") {
  AsrFixture f;
  const std::string clip = f.add_clip({{"source_in", "1s"}, {"duration", "3s"}}); // file seconds 1 to 4
  const json plain = f.run({{"project", f.project}, {"clip", clip}});
  REQUIRE(plain["state"] == "done");
  CHECK(plain["result"]["words"].size() == 3);

  REQUIRE(f.engine->call("timeline.edit", {{"project", f.project}, {"ops", json::array({{{"op", "set_speed"}, {"clip", clip}, {"speed", 2}}})}}));
  const json fast = f.run({{"project", f.project}, {"clip", clip}}); // the same 3 s of file, now played in 1.5 s
  REQUIRE(fast["state"] == "done");
  REQUIRE(fast["result"]["words"].size() == 3);
  CHECK(fast["result"]["words"][1]["start"].get<double>() == Catch::Approx(0.5)); // 1 s into the file is 0.5 s into the clip
  CHECK(fast["result"]["words"][2]["start"].get<double>() == Catch::Approx(1.0));
}

TEST_CASE("asr.transcribe: a file by path, with from and duration, and a language", "[asr][engine]") {
  AsrFixture f;
  const json part = f.run({{"path", f.tone}, {"from", 1.0}, {"duration", 2.0}, {"language", "ar"}});
  REQUIRE(part["state"] == "done");
  CHECK(part["result"]["words"].size() == 2);
  CHECK(part["result"]["language"] == "ar");
  CHECK_FALSE(part["result"].contains("clip"));
  CHECK(f.run({{"path", f.tone}})["result"]["words"].size() == 5);
}

TEST_CASE("asr.transcribe: what is wrong is said before a job starts", "[asr][engine]") {
  AsrFixture f;
  const std::string clip = f.add_clip();
  CHECK(f.refused({{"project", f.project}}) == "E_PARAM");                                         // nothing to listen to
  CHECK(f.refused({{"project", f.project}, {"clip", "clp_nope"}}) == "E_UNKNOWN_CLIP");
  CHECK(f.refused({{"project", f.project}, {"clip", clip}, {"language", "English!"}}) == "E_PARAM"); // a code, not a name
  CHECK(f.refused({{"path", (f.dir / "missing.wav").string()}}) != "");                            // no such file
  {
    Env gone;
    gone.set("ATTOME_WHISPER_EXE", (f.dir / "no-such-program.exe").string());
    CHECK(f.refused({{"path", f.tone}}) == "E_ASR_NOT_INSTALLED");
  }
  {
    Env gone;
    gone.set("ATTOME_WHISPER_MODEL", (f.dir / "no-such-model.bin").string()); // and nothing in the models folder
    CHECK(f.refused({{"path", f.tone}}) == "E_ASR_MODEL");
  }
}

TEST_CASE("asr.transcribe: a program that fails fails the job, with its message", "[asr][engine]") {
  AsrFixture f("error");
  const json failed = f.run({{"path", f.tone}});
  REQUIRE(failed["state"] == "failed");
  CHECK(failed["error"]["data"]["rule"] == "E_ASR_FAILED");
  CHECK(failed["error"]["message"].get<std::string>().find("the model is broken") != std::string::npos);
  f.set_model("crash");
  CHECK(f.run({{"path", f.tone}})["error"]["data"]["rule"] == "E_ASR_CRASH");
}

TEST_CASE("asr.transcribe: jobs.cancel stops a run that is going on", "[asr][engine]") {
  AsrFixture f("slow"); // the fake waits 30 s
  const auto started = f.engine->call("asr.transcribe", {{"path", f.tone}});
  REQUIRE(started);
  const std::string job = (*started)["job_id"];
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  REQUIRE(f.engine->call("jobs.cancel", {{"job_id", job}}));
  const auto begun = std::chrono::steady_clock::now();
  const json state = f.wait(job);
  CHECK(state["state"] == "cancelled");
  CHECK(std::chrono::steady_clock::now() - begun < std::chrono::seconds(10));
}

TEST_CASE("asr.transcribe: an agent can find it and the way to captions", "[asr][engine][parity]") {
  AsrFixture f;
  const json tools = *f.engine->call("tools.list", json::object());
  bool listed = false;
  for (const json &t : tools["tools"])
    listed = listed || t["name"] == "asr.transcribe";
  CHECK(listed);
  const std::string guide = (*f.engine->call("guide.get", {{"topic", "timeline"}}))["text"];
  CHECK(guide.find("asr.transcribe") != std::string::npos);
  CHECK(guide.find("words") != std::string::npos);
}
#endif
