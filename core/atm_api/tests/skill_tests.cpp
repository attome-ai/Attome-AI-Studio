#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "atm/api/engine.hpp"
#include "atm/base/id.hpp"

using atm::api::Engine;
using atm::api::json;
namespace fs = std::filesystem;

namespace {

json ok(Engine &e, const char *tool, json params) {
  auto r = e.call(tool, params);
  INFO(tool << ": " << (r ? "" : r.error().message));
  REQUIRE(r);
  return *r;
}

const json *find_skill(const json &list, const std::string &id) {
  for (const json &s : list)
    if (s.value("id", std::string()) == id)
      return &s;
  return nullptr;
}

} // namespace

TEST_CASE("Skills: the built-in ones are listed and readable, and a project keeps its own", "[skills]") {
  const fs::path dir = fs::temp_directory_path() / atm::new_id("attome-test");
  fs::create_directories(dir);
  const std::string project = (dir / "Skills.attome").string();
  Engine e;
  ok(e, "project.create", {{"path", project}, {"rate", "30"}});

  // Built in: no project needed. The entry point and its steps are there, with their recipe files.
  const json listed = ok(e, "skill.list", json::object());
  const json *entry = find_skill(listed["skills"], "short-video");
  REQUIRE(entry);
  CHECK(entry->value("source", std::string()) == "builtin");
  CHECK_FALSE(entry->value("description", std::string()).empty());
  const json *voice = find_skill(listed["skills"], "short-video-voice");
  REQUIRE(voice);
  CHECK((*voice)["files"].size() >= 5);

  const json got = ok(e, "skill.get", {{"id", "short-video"}});
  const std::string body = got.value("body", std::string());
  CHECK(body.find("short-video-concept") != std::string::npos);
  CHECK(body.rfind("---", 0) != 0); // the front matter is not part of the body

  // The niche skills: the rules and the default niche are built in, and the Short entry point sends the agent to them.
  CHECK(body.find("niche-rules") != std::string::npos);
  REQUIRE(find_skill(listed["skills"], "niche-rules"));
  REQUIRE(find_skill(listed["skills"], "niche-funny-short"));
  const json niche = ok(e, "skill.get", {{"id", "niche-funny-short"}});
  CHECK(niche.value("body", std::string()).find("CHECKS") != std::string::npos);

  const json file = ok(e, "skill.get", {{"id", "short-video-voice"}, {"file", "scripts/sync_captions_to_voice.ps1"}});
  const std::string text = file.value("text", std::string());
  CHECK(text.find("param(") != std::string::npos);
  CHECK(static_cast<unsigned char>(text[0]) != 0xEF); // no byte order mark

  CHECK_FALSE(e.call("skill.get", {{"id", "no-such-skill"}}));
  CHECK_FALSE(e.call("skill.get", {{"id", "short-video"}, {"file", "scripts/none.ps1"}}));

  // A project's own skill: one undoable edit each, listed next to the built-in ones, read, changed and removed.
  const json saved = ok(e, "skill.save", {{"project", project}, {"title", "Facts Short"}, {"description", "For fact videos"}, {"body", "1. Hook\n2. Facts"}});
  const std::string id = saved.value("skill", std::string());
  CHECK(id.rfind("skl_", 0) == 0);
  const json with_project = ok(e, "skill.list", {{"project", project}});
  const json *mine = find_skill(with_project["skills"], id);
  REQUIRE(mine);
  CHECK(mine->value("source", std::string()) == "project");
  CHECK(mine->value("title", std::string()) == "Facts Short");
  CHECK(find_skill(with_project["skills"], "short-video"));                 // the built-in ones stay listed
  CHECK_FALSE(find_skill(ok(e, "skill.list", json::object())["skills"], id)); // but this one belongs to the project only
  CHECK(ok(e, "skill.get", {{"id", id}, {"project", project}}).value("body", std::string()) == "1. Hook\n2. Facts");

  ok(e, "skill.save", {{"project", project}, {"id", id}, {"body", "1. Hook\n2. Facts\n3. Twist"}});
  CHECK(ok(e, "skill.get", {{"id", id}, {"project", project}}).value("body", std::string()) == "1. Hook\n2. Facts\n3. Twist");
  CHECK(ok(e, "skill.get", {{"id", id}, {"project", project}}).value("title", std::string()) == "Facts Short");

  ok(e, "project.undo", {{"project", project}}); // the change of the body is undone by one step
  CHECK(ok(e, "skill.get", {{"id", id}, {"project", project}}).value("body", std::string()) == "1. Hook\n2. Facts");

  CHECK(ok(e, "project.validate", {{"project", project}}).value("ok", false));
  CHECK_FALSE(e.call("skill.save", {{"project", project}, {"title", ""}, {"body", "x"}}));
  CHECK_FALSE(e.call("skill.delete", {{"project", project}, {"id", "short-video"}})); // built-in ones cannot be deleted
  ok(e, "skill.delete", {{"project", project}, {"id", id}});
  CHECK_FALSE(find_skill(ok(e, "skill.list", {{"project", project}})["skills"], id));

  std::error_code ec;
  fs::remove_all(dir, ec);
}
