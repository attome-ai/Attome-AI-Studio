#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "atm/api/engine.hpp"
#include "atm/base/id.hpp"
#include "atm/net/http.hpp"

namespace fs = std::filesystem;
using atm::api::Engine;
using atm::api::json;

namespace {

// Freesound in memory: a search with a CC0 sound and one that needs credit, each sound's facts, and the preview file.
struct Freesound final : atm::net::Transport {
  std::vector<std::string> asked;
  atm::Result<atm::net::Response> get(const atm::net::Request &request, const atm::net::OnResponse &on_response, const atm::net::Sink &sink) override {
    asked.push_back(request.url);
    const auto sound = [](int id, const char *license) {
      return json{{"id", id}, {"name", "boing " + std::to_string(id)}, {"duration", 1.234}, {"username", "maker"}, {"license", license},
                  {"tags", {"boing", "cartoon"}}, {"url", "https://freesound.org/people/maker/sounds/" + std::to_string(id) + "/"},
                  {"previews", {{"preview-hq-mp3", "https://cdn.freesound.org/previews/" + std::to_string(id) + "-hq.mp3"}}}};
    };
    std::string body;
    int status = 200;
    const std::string &u = request.url;
    if (u.find("token=good") == std::string::npos && u.find("cdn.freesound.org") == std::string::npos)
      status = 401;
    else if (u.find("/apiv2/search/") != std::string::npos)
      body = json{{"count", 2}, {"next", "https://freesound.org/apiv2/search/?page=2"},
                  {"results", {sound(11, "Creative Commons 0"), sound(12, "Attribution")}}}.dump();
    else if (u.find("/apiv2/sounds/11/") != std::string::npos)
      body = sound(11, "Creative Commons 0").dump();
    else if (u.find("/apiv2/sounds/12/") != std::string::npos)
      body = sound(12, "Attribution").dump();
    else if (u.find("/previews/11-hq.mp3") != std::string::npos)
      body = "ID3-fake-mp3";
    else
      status = 404;
    atm::net::Response r;
    r.status = status;
    r.content_length = int64_t(body.size());
    if (on_response && !on_response(r))
      return r;
    sink(reinterpret_cast<const uint8_t *>(body.data()), body.size());
    return r;
  }
};

} // namespace

TEST_CASE("sounds: Freesound search and download - the user's key, CC0 only, kept in the sound folder with its credit, used again from disk", "[sounds]") {
  const fs::path root = fs::temp_directory_path() / atm::new_id("attome-sounds");
  auto server = std::make_shared<Freesound>();
  atm::api::EngineConfig cfg;
  cfg.models_dir = (root / "models").string();
  cfg.transport = server;
  Engine engine(cfg);

  // no key yet: it says how to get one, and nothing is asked of the server
  const auto none = engine.call("sfx.search", {{"query", "boing"}});
  REQUIRE_FALSE(none);
  CHECK(none.error().rule == "S_NO_KEY");
  CHECK(server->asked.empty());
  CHECK_FALSE(engine.call("sfx.key", {{"key", "a&b"}}));

  // a refused key is said as such
  REQUIRE(engine.call("sfx.key", {{"key", "bad"}}));
  const auto refused = engine.call("sfx.search", {{"query", "boing"}});
  REQUIRE_FALSE(refused);
  CHECK(refused.error().rule == "S_KEY_REFUSED");

  const json key = engine.call("sfx.key", {{"key", " good "}}).value();
  CHECK(key["has_key"] == true);
  const json found = engine.call("sfx.search", {{"query", "cartoon boing"}, {"max_seconds", 3}}).value();
  INFO(found.dump());
  REQUIRE(found["results"].size() == 1); // the sound that needs credit stays out
  CHECK(found["results"][0]["id"] == 11);
  CHECK(found["results"][0]["license"] == "CC0");
  CHECK(found["results"][0]["seconds"] == 1.23);
  CHECK(found["results"][0]["downloaded"] == false);
  CHECK(found["more"] == true);
  const std::string asked = server->asked.back();
  CHECK(asked.find("query=cartoon%20boing") != std::string::npos);
  CHECK(asked.find("Creative%20Commons%200") != std::string::npos);
  CHECK(asked.find("duration%3A%5B0%20TO%203.00%5D") != std::string::npos);

  // download: the file and its credit in the sound folder; asked again, nothing is downloaded
  const json got = engine.call("sfx.fetch", {{"id", 11}}).value();
  const fs::path file = root / "sounds" / "freesound_11.mp3";
  CHECK(got["path"] == file.string());
  CHECK(got["cached"] == false);
  CHECK(got["author"] == "maker");
  CHECK(fs::file_size(file) == 12);
  CHECK(fs::exists(root / "sounds" / "freesound_11.json"));
  CHECK_FALSE(fs::exists(root / "sounds" / "freesound_11.mp3.part"));
  const size_t before = server->asked.size();
  const json again = engine.call("sfx.fetch", {{"id", "11"}, {"output", (root / "copy.mp3").string()}}).value();
  CHECK(again["cached"] == true);
  CHECK(server->asked.size() == before);
  CHECK(fs::file_size(root / "copy.mp3") == 12);
  CHECK(engine.call("sfx.search", {{"query", "boing"}}).value()["results"][0]["downloaded"] == true);

  // a sound that is not CC0 is refused, and so is a sound that is not there
  const auto credit = engine.call("sfx.fetch", {{"id", 12}});
  REQUIRE_FALSE(credit);
  CHECK(credit.error().rule == "S_LICENSE");
  CHECK_FALSE(fs::exists(root / "sounds" / "freesound_12.mp3"));
  const auto missing = engine.call("sfx.fetch", {{"id", 99}});
  REQUIRE_FALSE(missing);
  CHECK(missing.error().rule == "S_NOT_FOUND");
  CHECK_FALSE(engine.call("sfx.fetch", {{"id", "x"}}));

  const json list = engine.call("sfx.downloaded", json::object()).value();
  REQUIRE(list["sounds"].size() == 1);
  CHECK(list["sounds"][0]["name"] == "boing 11");
  CHECK(list["has_key"] == true);
  std::error_code ec;
  fs::remove_all(root, ec);
}
