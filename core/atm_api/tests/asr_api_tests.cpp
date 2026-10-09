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
  std::string root_id() {
    const json info = *engine->call("project.inspect", {{"project", project}});
    return info["data"]["id"];
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

TEST_CASE("asr.transcribe: what was heard is kept in the project, in the file's time, and answers the next ask at once", "[asr][engine]") {
  AsrFixture f;
  const std::string clip = f.add_clip({{"source_in", "1s"}, {"duration", "3s"}}); // file seconds 1 to 4
  const json first = f.run({{"project", f.project}, {"clip", clip}});
  REQUIRE(first["state"] == "done");
  CHECK(first["result"]["cached"] == false);
  REQUIRE(first["result"]["words"].size() == 3);

  // The next call of the engine puts it in the project: one "transcripts" item, with the words in the file's time (1 s, 2 s, 3 s).
  const json root = f.engine->call("project.get", {{"project", f.project}, {"id", f.root_id()}})->at("object");
  REQUIRE(root.contains("transcripts"));
  REQUIRE(root["transcripts"].size() == 1);
  const json kept = *root["transcripts"].begin();
  CHECK(kept["media"] == f.tone);
  CHECK(kept["asked"] == "auto");
  CHECK(kept["from"].get<double>() == Catch::Approx(1.0));
  CHECK(kept["to"].get<double>() == Catch::Approx(4.0));
  REQUIRE(kept["words"].size() == 3);
  CHECK(kept["words"][0]["start"].get<double>() == Catch::Approx(1.0));
  CHECK(kept["words"][2]["start"].get<double>() == Catch::Approx(3.0));
  CHECK(f.engine->call("project.validate", {{"project", f.project}})->at("ok") == true);

  // From now on the program is not needed for that part: with one that fails, the answer is the same and comes at once.
  f.set_model("crash");
  const json again = f.run({{"project", f.project}, {"clip", clip}});
  REQUIRE(again["state"] == "done");
  CHECK(again["result"]["cached"] == true);
  CHECK(again["result"]["words"] == first["result"]["words"]);

  // The clip may be trimmed, moved, split or sped up: its words are the ones of the part of the file it plays now, in its own time.
  REQUIRE(f.engine->call("timeline.edit", {{"project", f.project}, {"ops", json::array({{{"op", "trim"}, {"clip", clip}, {"edge", "in"}, {"delta", "1s"}}})}}));
  const json trimmed = f.run({{"project", f.project}, {"clip", clip}}); // now file seconds 2 to 4
  REQUIRE(trimmed["state"] == "done");
  CHECK(trimmed["result"]["cached"] == true);
  REQUIRE(trimmed["result"]["words"].size() == 2);
  CHECK(trimmed["result"]["words"][0]["start"].get<double>() == Catch::Approx(0.0)); // the word at 2 s of the file is the first of the clip
  CHECK(trimmed["result"]["words"][1]["start"].get<double>() == Catch::Approx(1.0));
  CHECK(f.run({{"path", f.tone}, {"project", f.project}, {"from", 2.0}, {"duration", 1.0}})["result"]["cached"] == true); // by path too

  // A part that is not covered is listened to, and the one transcript grows to hold both (not a second item).
  f.set_model("ok");
  const json more = f.run({{"path", f.tone}, {"project", f.project}, {"from", 3.0}, {"duration", 2.0}}); // 3 s to 5 s: 4 s of the file is new
  REQUIRE(more["state"] == "done");
  CHECK(more["result"]["cached"] == false);
  const json grown = f.engine->call("project.get", {{"project", f.project}, {"id", f.root_id()}})->at("object")["transcripts"];
  // (the engine puts it in the project at its next call: any call will do)
  const json grown2 = f.engine->call("project.get", {{"project", f.project}, {"id", f.root_id()}})->at("object")["transcripts"];
  REQUIRE(grown2.size() == 1);
  const json after = *grown2.begin();
  CHECK(after["from"].get<double>() == Catch::Approx(1.0));
  CHECK(after["to"].get<double>() == Catch::Approx(5.0));
  CHECK(after["words"].size() == 4); // 1 s to 5 s: one word for each second the program was given
  (void)grown;

  // again: true listens anew even though it is all there; another language asked is another transcript; no project, nothing kept.
  f.set_model("crash");
  CHECK(f.run({{"project", f.project}, {"clip", clip}, {"again", true}})["state"] == "failed");
  f.set_model("ok");
  CHECK(f.run({{"project", f.project}, {"clip", clip}, {"language", "ar"}})["result"]["cached"] == false);
  CHECK(f.run({{"path", f.tone}})["result"]["cached"] == false);
  CHECK(f.engine->call("project.get", {{"project", f.project}, {"id", f.root_id()}})->at("object")["transcripts"].size() == 2);
}

TEST_CASE("asr.transcribe: a file that changed is listened to again", "[asr][engine]") {
  AsrFixture f;
  const json first = f.run({{"project", f.project}, {"path", f.tone}});
  REQUIRE(first["state"] == "done");
  CHECK(f.run({{"project", f.project}, {"path", f.tone}})["result"]["cached"] == true);
  { // the same name, another sound (another size)
    std::vector<float> pcm(size_t(3) * 48000 * 2, 0.2f);
    REQUIRE(audio::write_wav(f.tone, pcm));
  }
  const json changed = f.run({{"project", f.project}, {"path", f.tone}});
  REQUIRE(changed["state"] == "done");
  CHECK(changed["result"]["cached"] == false);
  CHECK(changed["result"]["words"].size() == 3);
}

// A model of the catalog counts as installed when a file of its exact size is under its name: the fake program ignores what is inside.
void put_model(const fs::path &models, const char *name, uintmax_t size) {
  fs::create_directories(models);
  std::ofstream(models / name, std::ios::binary).put('x');
  fs::resize_file(models / name, size);
}

TEST_CASE("asr.transcribe: the more exact model is used when it is there, and a model can be asked for", "[asr][engine]") {
  AsrFixture f;
  f.env.set("ATTOME_WHISPER_MODEL", ""); // no file of the user's own: the catalog's models
  const fs::path models = f.dir / "models";
  const json params = {{"path", f.tone}};
  const auto with = [&](json extra) {
    json p = params;
    for (auto it = extra.begin(); it != extra.end(); ++it)
      p[it.key()] = it.value();
    return p;
  };

  CHECK(f.refused(params) == "E_ASR_MODEL"); // none at all
  const auto none = f.engine->call("asr.transcribe", params);
  REQUIRE_FALSE(none);
  CHECK(none.error().hint.find("whisper.large-v3-turbo-q5") != std::string::npos); // it says both can be had

  put_model(models, "ggml-small.bin", 487601967);
  const json small = f.run(params);
  REQUIRE(small["state"] == "done");
  CHECK(small["result"]["model"] == "whisper.small");
  CHECK(f.refused(with({{"model", "turbo"}})) == "E_ASR_MODEL"); // asked for the one that is not there
  CHECK(f.refused(with({{"model", "big"}})) == "E_PARAM");

  put_model(models, "ggml-large-v3-turbo-q5_0.bin", 574041195);
  CHECK(f.run(with({{"again", true}}))["result"]["model"] == "whisper.large-v3-turbo-q5");                       // the better one by default
  CHECK(f.run(with({{"again", true}, {"model", "small"}}))["result"]["model"] == "whisper.small");               // or the other, asked for
  CHECK(f.run(with({{"again", true}, {"model", "turbo"}}))["result"]["model"] == "whisper.large-v3-turbo-q5");
  f.set_model("ok"); // a file of the user's own beats both, and says so
  f.env.set("ATTOME_WHISPER_MODEL", (f.dir / "model.bin").string());
  CHECK(f.run(with({{"again", true}}))["result"]["model"] == "custom");
}

TEST_CASE("asr.transcribe: the graphics card by default, the processor when asked, and the result says which", "[asr][engine]") {
  AsrFixture f;
  const json by_default = f.run({{"path", f.tone}});
  REQUIRE(by_default["state"] == "done");
  CHECK(by_default["result"]["device"] == "gpu");
  CHECK(f.run({{"path", f.tone}, {"device", "cpu"}})["result"]["device"] == "cpu");
  CHECK(f.run({{"path", f.tone}, {"device", "gpu"}})["result"]["device"] == "gpu");
  CHECK(f.refused({{"path", f.tone}, {"device", "npu"}}) == "E_PARAM");
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
