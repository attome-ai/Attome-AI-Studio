#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <thread>

#include "atm/api/engine.hpp"
#include "atm/api/server.hpp"
#include "atm/base/id.hpp"
#include "atm/storage/file.hpp"

using atm::api::Engine;
using atm::api::json;
namespace fs = std::filesystem;

namespace {

struct TempDir {
  fs::path path = fs::temp_directory_path() / atm::new_id("attome-test");
  TempDir() { fs::create_directories(path); }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  std::string project() const { return (path / "Demo.attome").string(); }
};

json ok(Engine &e, const char *tool, json params) {
  auto r = e.call(tool, params);
  INFO(tool << ": " << (r ? "" : r.error().message));
  REQUIRE(r);
  return *r;
}

atm::Error err(Engine &e, const char *tool, json params) {
  auto r = e.call(tool, params);
  REQUIRE_FALSE(r);
  return r.error();
}

json clip(const char *in, const char *dur, const char *name = "clip") {
  return {{"name", name}, {"timing", {{"record_in", in}, {"duration", dur}, {"source_in", "0s"}}},
          {"media_ref", {{"type", "file"}, {"path", "media/a.mov"}}}};
}

// Creates a project with one track and returns {sequence, track}.
std::pair<std::string, std::string> setup(Engine &e, const std::string &project) {
  const json created = ok(e, "project.create", {{"path", project}, {"rate", "30000/1001"}});
  const std::string seq = created["sequence"];
  const json added = ok(e, "project.patch",
                        {{"project", project},
                         {"patch", {{"ops", json::array({{{"op", "add"},
                                                          {"path", seq + "/tracks/$new:v1"},
                                                          {"value", {{"kind", "video"}, {"name", "V1"},
                                                                     {"clips", json::object()},
                                                                     {"clip_order", json::array()}}}}})}}}});
  return {seq, added["id_map"]["$new:v1"]};
}

json patch_of(const std::string &project, json ops) { return {{"project", project}, {"patch", {{"ops", std::move(ops)}}}}; }

std::string canonical(Engine &e, const std::string &project) {
  ok(e, "project.save", {{"project", project}});
  return *atm::storage::read_file(fs::path(project) / "project.json");
}

} // namespace

TEST_CASE("patch: add, normalize times, undo and redo restore the exact document", "[engine]") {
  TempDir tmp;
  const std::string project = tmp.project();
  Engine e;
  const auto [seq, trk] = setup(e, project);
  const std::string before = canonical(e, project);

  const json r = ok(e, "project.patch",
                    patch_of(project, json::array({{{"op", "add"}, {"path", trk + "/clips/$new:a"},
                                                    {"value", clip("2.5s", "00:00:03:00", "Intro")}},
                                                   {{"op", "add"}, {"path", trk + "/clips/$new:b"},
                                                    {"anchor", {{"before", "$new:a"}}},
                                                    {"value", clip("0s", "60@24", "Cold open")}},
                                                   {{"op", "replace"}, {"path", "$new:a/transform/opacity"},
                                                    {"value", 0.8}}})));
  const std::string a = r["id_map"]["$new:a"], b = r["id_map"]["$new:b"];
  CHECK(r["applied"] == true);
  CHECK(atm::id_prefix(a) == "clp");

  const json got = ok(e, "project.get", {{"project", project}, {"id", a}})["object"];
  CHECK(got["timing"]["record_in"] == "5/2");
  CHECK(got["timing"]["duration"] == "3003/1000"); // 90 frames at 29.97
  CHECK(got["transform"]["opacity"] == 0.8);
  const json track = ok(e, "project.get", {{"project", project}, {"id", trk}})["object"];
  CHECK(track["clip_order"] == json::array({b, a}));
  CHECK(ok(e, "project.validate", {{"project", project}})["ok"] == true);
  const std::string after = canonical(e, project);
  CHECK(after != before);

  ok(e, "project.undo", {{"project", project}});
  CHECK(canonical(e, project) == before);
  ok(e, "project.redo", {{"project", project}});
  CHECK(canonical(e, project) == after);
  CHECK(err(e, "project.redo", {{"project", project}}).rule == "R_HISTORY_END");
}

TEST_CASE("patch: all-or-nothing, with agent-readable errors", "[engine]") {
  TempDir tmp;
  const std::string project = tmp.project();
  Engine e;
  const auto [seq, trk] = setup(e, project);
  ok(e, "project.patch", patch_of(project, json::array({{{"op", "add"}, {"path", trk + "/clips/$new:a"},
                                                         {"value", clip("0s", "3s")}}})));
  const std::string before = canonical(e, project);
  const uint64_t revision = ok(e, "project.inspect", {{"project", project}})["revision"];

  // The second op overlaps the first clip: nothing of the patch may stay.
  const atm::Error overlap =
      err(e, "project.patch",
          patch_of(project, json::array({{{"op", "replace"}, {"path", seq + "/name"}, {"value", "Renamed"}},
                                         {{"op", "add"}, {"path", trk + "/clips/$new:b"},
                                          {"value", clip("2s", "3s")}}})));
  CHECK(overlap.code == atm::ErrorCode::SchemaViolation);
  CHECK(overlap.rule == "R_TRACK_OVERLAP");
  CHECK(overlap.hint.find("record_in to \"3\"") != std::string::npos);
  CHECK(canonical(e, project) == before);
  CHECK(ok(e, "project.inspect", {{"project", project}})["revision"] == revision);

  CHECK(err(e, "project.patch", patch_of(project, json::array({{{"op", "replace"}, {"path", "clp_nope/name"},
                                                                {"value", 1}}})))
            .code == atm::ErrorCode::UnknownId);
  CHECK(err(e, "project.patch", patch_of(project, json::array({{{"op", "test"}, {"path", seq + "/name"},
                                                                {"value", "Other"}}})))
            .rule == "P_TEST_FAILED");
  CHECK(err(e, "project.patch", patch_of(project, json::array({{{"op", "add"}, {"path", seq + "/tracks/$new:t"},
                                                                {"value", {{"kind", "video"}}},
                                                                {"anchor", {{"after", "trk_missing"}}}}})))
            .rule == "P_ANCHOR");
  CHECK(canonical(e, project) == before);

  // Dry run: reports the normalized patch and its inverse, changes nothing.
  json dry = patch_of(project, json::array({{{"op", "add"}, {"path", trk + "/clips/$new:c"},
                                             {"value", clip("10s", "1s")}}}));
  dry["dry_run"] = true;
  const json preview = ok(e, "project.patch", dry);
  CHECK(preview["applied"] == false);
  CHECK(preview["patch"]["ops"][0]["value"]["timing"]["record_in"] == "10");
  CHECK(preview["inverse"]["ops"][0]["op"] == "remove");
  CHECK(canonical(e, project) == before);
}

TEST_CASE("patch: move, remove and reorder are undone exactly", "[engine]") {
  TempDir tmp;
  const std::string project = tmp.project();
  Engine e;
  const auto [seq, v1] = setup(e, project);
  const json r = ok(e, "project.patch",
                    patch_of(project, json::array({{{"op", "add"}, {"path", seq + "/tracks/$new:v2"},
                                                    {"value", {{"kind", "video"}, {"name", "V2"}}}},
                                                   {{"op", "add"}, {"path", v1 + "/clips/$new:a"},
                                                    {"value", clip("0s", "2s", "A")}},
                                                   {{"op", "add"}, {"path", v1 + "/clips/$new:b"},
                                                    {"value", clip("2s", "2s", "B")}},
                                                   {{"op", "add"}, {"path", v1 + "/clips/$new:c"},
                                                    {"value", clip("4s", "2s", "C")}}})));
  const std::string v2 = r["id_map"]["$new:v2"], a = r["id_map"]["$new:a"], b = r["id_map"]["$new:b"];
  const std::string before = canonical(e, project);

  ok(e, "project.patch", patch_of(project, json::array({{{"op", "move"}, {"path", b}, {"to", v2 + "/clips"}},
                                                        {{"op", "remove"}, {"path", a}},
                                                        {{"op", "move"}, {"path", v2}, {"to", seq + "/tracks"},
                                                         {"anchor", {{"first", true}}}}})));
  const json sequence = ok(e, "project.get", {{"project", project}, {"id", seq}})["object"];
  CHECK(sequence["track_order"] == json::array({v2, v1}));
  CHECK(sequence["tracks"][v2]["clip_order"] == json::array({b}));
  CHECK(sequence["tracks"][v1]["clips"].size() == 1);
  CHECK(err(e, "project.get", {{"project", project}, {"id", a}}).code == atm::ErrorCode::UnknownId);
  CHECK(ok(e, "project.get", {{"project", project}, {"id", b}})["parent"] == v2);

  ok(e, "project.undo", {{"project", project}});
  CHECK(canonical(e, project) == before);
  CHECK(ok(e, "project.get", {{"project", project}, {"id", a}})["parent"] == v1);
  CHECK(err(e, "project.patch", patch_of(project, json::array({{{"op", "move"}, {"path", v1},
                                                                {"to", v1 + "/clips"}}})))
            .code != atm::ErrorCode::Ok); // a track is not a clip, and cannot move into itself
}

TEST_CASE("journal: edits acknowledged before a crash are recovered", "[engine]") {
  TempDir tmp;
  const std::string project = tmp.project();
  std::string track, expected;
  {
    Engine e;
    track = setup(e, project).second;
    ok(e, "project.save", {{"project", project}});
    ok(e, "project.patch", patch_of(project, json::array({{{"op", "add"}, {"path", track + "/clips/$new:a"},
                                                           {"value", clip("0s", "1s", "A")}}})));
    ok(e, "project.patch", patch_of(project, json::array({{{"op", "add"}, {"path", track + "/clips/$new:b"},
                                                           {"value", clip("1s", "1s", "B")}}})));
    ok(e, "project.patch", patch_of(project, json::array({{{"op", "add"}, {"path", track + "/clips/$new:c"},
                                                           {"value", clip("2s", "1s", "C")}}})));
    ok(e, "project.undo", {{"project", project}});
    expected = ok(e, "project.get", {{"project", project}, {"id", track}})["object"].dump();
    // The engine goes away without saving: project.json still has no clips.
  }
  {
    Engine e;
    const json info = ok(e, "project.inspect", {{"project", project}});
    CHECK(info["data"]["recovered"] == 4); // three edits and one undo
    CHECK(ok(e, "project.get", {{"project", project}, {"id", track}})["object"].dump() == expected);
    ok(e, "project.redo", {{"project", project}}); // the undo tree survived too
    CHECK(ok(e, "project.get", {{"project", project}, {"id", track}})["object"]["clips"].size() == 3);
    CHECK(ok(e, "history.list", {{"project", project}})["changesets"].size() == 4);
  }
  { // A torn last record is cut off; everything before it still replays.
    const fs::path journal = fs::path(project) / ".attome" / "journal" / "000001.jnl";
    std::ofstream(journal, std::ios::binary | std::ios::app) << "\x40\x00\x00\x00torn";
    Engine e;
    CHECK(ok(e, "project.get", {{"project", project}, {"id", track}})["object"]["clips"].size() == 3);
  }
}

TEST_CASE("project lock: a second engine cannot open the same project", "[engine]") {
  TempDir tmp;
  const std::string project = tmp.project();
  Engine first;
  setup(first, project);
  Engine second;
  CHECK(err(second, "project.inspect", {{"project", project}}).code == atm::ErrorCode::ProjectLocked);
  ok(first, "project.close", {{"project", project}});
  CHECK(ok(second, "project.inspect", {{"project", project}})["data"]["objects"] == 3);
}

TEST_CASE("daemon: JSON-RPC over the transport", "[engine]") {
  TempDir tmp;
  const std::string project = tmp.project();
#if defined(_WIN32)
  const std::string endpoint = "\\\\.\\pipe\\" + atm::new_id("attome-test");
#else
  const std::string endpoint = (tmp.path / "d.sock").string();
#endif
  Engine engine;
  {
    atm::api::Server server(engine);
    auto listener = atm::api::Listener::listen(endpoint);
    REQUIRE(listener);
    CHECK_FALSE(atm::api::Listener::listen(endpoint)); // one daemon per endpoint
    std::thread accept([&] { server.serve(*listener); });

    auto stream = atm::api::connect(endpoint);
    REQUIRE(stream);
    atm::api::FrameReader reader(*stream);
    std::string body;
    const auto rpc = [&](json request) {
      REQUIRE(atm::api::write_frame(*stream, request.dump()));
      REQUIRE(reader.read(body));
      return json::parse(body);
    };
    const json created = rpc({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "project.create"},
                              {"params", {{"path", project}}}});
    CHECK(created["id"] == 1);
    CHECK(atm::id_prefix(created["result"]["project"].get<std::string>()) == "prj");
    CHECK(rpc({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "no.such.tool"}})["error"]["code"] == -32601);
    const json bad = rpc({{"jsonrpc", "2.0"}, {"id", 3}, {"method", "project.get"},
                          {"params", {{"project", project}, {"id", "clp_missing"}}}});
    CHECK(bad["error"]["code"] == 1002);
    CHECK(bad["error"]["data"]["rule"] == "P_UNKNOWN_ID");
    const json profile = rpc({{"jsonrpc", "2.0"}, {"id", 4}, {"method", "profile.get"}});
    CHECK(profile["result"]["threads"].is_array());

    stream.reset();
    listener->close();
    accept.join();
  }
}
