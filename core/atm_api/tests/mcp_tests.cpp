#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "atm/api/engine.hpp"
#include "atm/api/mcp.hpp"
#include "atm/base/id.hpp"
#include "atm/storage/file.hpp"

using atm::api::Engine;
using atm::api::json;
using atm::api::McpServer;
namespace fs = std::filesystem;

namespace {

struct TempDir {
  fs::path path = fs::temp_directory_path() / atm::new_id("attome-mcp");
  TempDir() { fs::create_directories(path); }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

atm::api::ToolCaller caller(Engine &engine) {
  return [&engine](const std::string &tool, const json &params) -> json {
    auto r = engine.call(tool, params);
    if (r)
      return {{"ok", true}, {"result", std::move(*r)}};
    return {{"ok", false}, {"error", atm::error_to_json(r.error())}};
  };
}

fs::path path_of(const json &image) {
  const std::string s = image["path"];
  return fs::path(std::u8string(s.begin(), s.end()));
}

json request(int id, const char *method, json params = json::object()) {
  return {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}};
}

json call_tool(McpServer &mcp, const char *name, json args) {
  const json response = mcp.handle(request(9, "tools/call", {{"name", name}, {"arguments", std::move(args)}}));
  INFO(response.dump());
  REQUIRE(response.contains("result"));
  return response["result"];
}

// A project with a 2-second white title on a "Titles" track: renders without any media file.
std::string titled_project(Engine &engine, const fs::path &dir) {
  const std::string project = (dir / "Demo.attome").string();
  auto created = engine.call("project.create", {{"path", project}, {"canvas", {{"width", 640}, {"height", 360}}}});
  REQUIRE(created);
  const std::string seq = (*created)["sequence"];
  const json ops = json::array(
      {{{"op", "add"}, {"path", seq + "/tracks/$new:t"}, {"value", {{"kind", "video"}, {"name", "Titles"}}}},
       {{"op", "add"},
        {"path", "$new:t/clips/$new:c"},
        {"value",
         {{"name", "Title"},
          {"timing", {{"record_in", "0s"}, {"duration", "2s"}, {"source_in", "0s"}}},
          {"media_ref", {{"type", "text"}}},
          {"content", {{"text", "Hello"}, {"size", 0.3}, {"color", "#ffffff"}, {"bold", true}}}}}}});
  auto patched = engine.call("project.patch", {{"project", project}, {"patch", {{"ops", ops}}}});
  INFO((patched ? "" : patched.error().message));
  REQUIRE(patched);
  return project;
}

} // namespace

TEST_CASE("mcp: handshake, tool list from the registry, and errors as tool results", "[mcp]") {
  Engine engine;
  McpServer mcp(caller(engine));

  const json init = mcp.handle(request(1, "initialize", {{"protocolVersion", "2025-03-26"}, {"capabilities", json::object()},
                                                         {"clientInfo", {{"name", "test"}, {"version", "1"}}}}));
  REQUIRE(init["result"]["protocolVersion"] == "2025-03-26");
  REQUIRE(init["result"]["serverInfo"]["name"] == "attome");
  REQUIRE(init["result"]["capabilities"].contains("tools"));
  CHECK(mcp.handle(request(1, "initialize", {{"protocolVersion", "1999-01-01"}}))["result"]["protocolVersion"] ==
        "2025-06-18");
  CHECK(mcp.handle({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}).is_null());
  CHECK(mcp.handle(request(2, "ping"))["result"] == json::object());
  CHECK(mcp.handle(request(3, "resources/list"))["error"]["code"] == -32601);

  const json tools = mcp.handle(request(4, "tools/list"))["result"]["tools"];
  REQUIRE(tools.is_array());
  std::map<std::string, json> by_name;
  for (const json &t : tools) {
    REQUIRE(t["inputSchema"]["type"] == "object");
    REQUIRE_FALSE(t.value("description", "").empty());
    by_name[t["name"]] = t;
  }
  for (const char *name : {"project_create", "project_patch", "project_inspect", "media_probe", "see_frames",
                           "see_contact_sheet", "render_sequence", "jobs_get", "jobs_wait"})
    CHECK(by_name.count(name) == 1);
  CHECK(by_name.count("daemon_shutdown") == 0); // the daemon's controls are not for agents
  CHECK(by_name.count("tools_list") == 0);
  CHECK(by_name["project_patch"]["inputSchema"]["required"] == json::array({"project", "patch"}));
  CHECK(by_name["project_inspect"]["annotations"]["readOnlyHint"] == true);
  CHECK(by_name["project_patch"]["annotations"]["readOnlyHint"] == false);

  // MCP clients cut long descriptions (an agent once missed the keyframe format that way), so keep them short and
  // put the recipes in guide_get.
  for (const json &t : tools)
    CHECK(t["description"].get<std::string>().size() <= 1024);
  const std::string guide = json::parse(call_tool(mcp, "guide_get", json::object())["content"][0]["text"].get<std::string>())["text"];
  for (const char *topic : {"## clips", "## text", "## dissolves", "## keyframes", "## effects", "## audio", "## times", "in_offset", "ease_out_cubic"})
    CHECK(guide.find(topic) != std::string::npos);
  const std::string one = json::parse(call_tool(mcp, "guide_get", {{"topic", "keyframes"}})["content"][0]["text"].get<std::string>())["text"];
  CHECK(one.find("## dissolves") == std::string::npos);
  CHECK(call_tool(mcp, "guide_get", {{"topic", "music"}})["isError"] == true);

  const json missing = call_tool(mcp, "project_inspect", {{"project", "C:/no/such/place.attome"}});
  CHECK(missing["isError"] == true);
  CHECK(missing["content"][0]["text"].get<std::string>().find("Hint:") != std::string::npos);
  CHECK(mcp.handle(request(5, "tools/call", {{"name", "nope"}}))["error"]["code"] == -32602);
  CHECK(call_tool(mcp, "jobs_wait", {{"job_id", "job_unknown"}})["isError"] == true);
}

TEST_CASE("mcp: an agent builds a project and sees it", "[mcp][media]") {
  TempDir tmp;
  Engine engine;
  McpServer mcp(caller(engine));
  const std::string project = titled_project(engine, tmp.path);

  const json inspected = call_tool(mcp, "project_inspect", {{"project", project}, {"level", "tracks"}});
  CHECK_FALSE(inspected.contains("isError"));
  CHECK(json::parse(inspected["content"][0]["text"].get<std::string>())["data"]["sequences"][0]["tracks"][0]["clips"] == 1);

  const json frames = call_tool(mcp, "see_frames", {{"project", project}, {"times", {"1s", "10s"}}, {"height", 180}});
  INFO(frames["content"][0]["text"]);
  REQUIRE_FALSE(frames.contains("isError"));
  REQUIRE(frames["content"].size() == 3); // the JSON, then one picture per time
  CHECK(frames["content"][1]["type"] == "image");
  CHECK(frames["content"][1]["mimeType"] == "image/jpeg");
  CHECK(frames["content"][1]["data"].get<std::string>().rfind("/9j/", 0) == 0); // base64 of the JPEG SOI marker
  const json info = json::parse(frames["content"][0]["text"].get<std::string>());
  CHECK(info["width"] == 320);
  CHECK(info["images"][0]["frame"] == 30);
  CHECK(info["images"][1]["frame"] == 59); // past the end: the last frame
  CHECK(fs::exists(path_of(info["images"][0])));

  const json sheet = call_tool(mcp, "see_contact_sheet", {{"project", project}, {"count", 6}, {"columns", 3}});
  REQUIRE_FALSE(sheet.contains("isError"));
  REQUIRE(sheet["content"].size() == 2);
  const json sheet_info = json::parse(sheet["content"][0]["text"].get<std::string>());
  CHECK(sheet_info["tiles"].size() == 6);
  CHECK(sheet_info["columns"] == 3);
  // The earlier pictures were replaced, so the folder does not grow.
  CHECK_FALSE(fs::exists(path_of(info["images"][0])));
}

TEST_CASE("see: bad parameters and an empty sequence are refused with a hint", "[mcp]") {
  TempDir tmp;
  Engine engine;
  const std::string project = (tmp.path / "Empty.attome").string();
  REQUIRE(engine.call("project.create", {{"path", project}}));
  auto empty = engine.call("see.contact_sheet", {{"project", project}});
  REQUIRE_FALSE(empty);
  CHECK(empty.error().rule == "R_EMPTY");
  auto no_times = engine.call("see.frames", {{"project", project}, {"times", json::array()}});
  REQUIRE_FALSE(no_times);
  CHECK(no_times.error().rule == "E_PARAM");
}
