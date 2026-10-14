// Sound effects from the internet: sfx.search looks through Freesound's recordings (only those under CC0, which need no credit and allow any
// use), sfx.fetch downloads one into the user's sound folder (and the project, when one is given), sfx.downloaded lists what is there, and
// sfx.key keeps the user's own Freesound API key. Attome ships no key: each user asks Freesound for one (free) and gives it once.
#include "engine_impl.hpp"

#include <cstdio>
#include <fstream>

#include "atm/net/http.hpp"

namespace atm::api {

namespace {

constexpr const char *kApi = "https://freesound.org/apiv2/";
constexpr const char *kCc0 = "Creative Commons 0";
constexpr const char *kKeyHint = "Ask Freesound for a free API key at https://freesound.org/apiv2/apply (a Freesound account is needed), "
                                 "then give it once with sfx.key {\"key\": \"...\"}; or set ATTOME_FREESOUND_KEY.";

std::string url_encode(const std::string &s) {
  std::string out;
  for (const unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += char(c);
    } else {
      char hex[4];
      std::snprintf(hex, sizeof hex, "%%%02X", unsigned(c));
      out += hex;
    }
  }
  return out;
}

std::string file_stem(int64_t id) { return "freesound_" + std::to_string(id); }

  // What a sound of Freesound's answer becomes in ours: the facts an editor or an agent chooses by, and the credit line.
json sound_of(const json &s) {
  json tags = json::array();
  if (s.contains("tags") && s["tags"].is_array())
    for (const json &t : s["tags"])
      if (t.is_string() && tags.size() < 8)
        tags.push_back(t);
  std::string preview;
  if (s.contains("previews") && s["previews"].is_object())
    preview = s["previews"].value("preview-hq-mp3", s["previews"].value("preview-lq-mp3", std::string()));
  const int64_t id = s.value("id", int64_t(0));
  return {{"id", id},
          {"name", s.value("name", std::string())},
          {"seconds", std::round(s.value("duration", 0.0) * 100.0) / 100.0},
          {"author", s.value("username", std::string())},
          {"license", s.value("license", std::string()) == kCc0 || s.value("license", std::string()).find("/zero/") != std::string::npos ? "CC0" : s.value("license", std::string())},
          {"tags", std::move(tags)},
          {"preview", preview},
          {"url", s.value("url", "https://freesound.org/s/" + std::to_string(id) + "/")}};
}

Result<json> ask(net::Transport &transport, const std::string &url) {
  ATM_TRY(net::Reply reply, net::fetch(transport, url));
  if (reply.status == 401 || reply.status == 403)
    return fail(ErrorCode::PolicyPermissionDenied, "S_KEY_REFUSED", "Freesound refused the API key.", {}, kKeyHint);
  if (reply.status == 404)
    return fail(ErrorCode::NotFound, "S_NOT_FOUND", "Freesound has no such sound.");
  if (reply.status == 429)
    return fail(ErrorCode::ProviderRateLimited, "S_RATE_LIMIT", "Freesound says too many requests; try again in a minute.");
  if (reply.status != 200)
    return fail(ErrorCode::ProviderError, "S_HTTP", "Freesound answered " + std::to_string(reply.status) + ".");
  json body = json::parse(reply.body, nullptr, false);
  if (!body.is_object())
    return fail(ErrorCode::ProviderError, "S_BAD_ANSWER", "Freesound's answer is not JSON.");
  return body;
}

} // namespace

std::string Engine::Impl::freesound_key() const {
  if (const char *key = std::getenv("ATTOME_FREESOUND_KEY"); key && *key)
    return key;
  if (!freesound_key_set.empty())
    return freesound_key_set;
  const json all = settings();
  return all.contains("freesound_key") && all["freesound_key"].is_string() ? all["freesound_key"].get<std::string>() : std::string();
}

  // Downloaded sounds live beside the models and tools (ATTOME_SOUNDS_DIR moves them): every project can use them.
fs::path Engine::Impl::sounds_dir() const {
  if (const char *dir = std::getenv("ATTOME_SOUNDS_DIR"); dir && *dir)
    return to_path(dir);
  return models_dir().parent_path() / "sounds";
}

  // sfx.key {key}: keeps the user's Freesound API key in the settings ("" takes it away). The key is never given back.
Result<json> Engine::Impl::sfx_key(const json &params) {
  ATM_TRY(const std::string *key, string_param(params, "key"));
  std::string k = *key;
  while (!k.empty() && std::isspace(static_cast<unsigned char>(k.back())))
    k.pop_back();
  while (!k.empty() && std::isspace(static_cast<unsigned char>(k.front())))
    k.erase(k.begin());
  if (k.find_first_of("&?=/# ") != std::string::npos)
    return bad_param("key", "must be the key Freesound gave, letters and digits only");
  freesound_key_set = k;
  bool saved = false;
  if (const fs::path path = settings_path(); !path.empty()) {
    json all = settings();
    if (k.empty())
      all.erase("freesound_key");
    else
      all["freesound_key"] = k;
    ATM_CHECK(storage::make_dirs(path.parent_path()));
    ATM_CHECK(storage::atomic_write(path, all.dump(2) + "\n"));
    saved = true;
  }
  return json{{"has_key", !freesound_key().empty()}, {"saved", saved}};
}

  // sfx.search {query, page?, page_size?, max_seconds?}: Freesound's CC0 recordings that match, best first.
Result<json> Engine::Impl::sfx_search(const json &params) {
  ATM_PROFILE_SCOPE("api.sfx_search");
  ATM_TRY(const std::string *query, string_param(params, "query"));
  if (query->empty())
    return bad_param("query", "must say what to look for (\"cartoon boing\", \"egg crack\")");
  const std::string key = freesound_key();
  if (key.empty())
    return fail(ErrorCode::PolicyConsentMissing, "S_NO_KEY", "No Freesound API key has been given.", {}, kKeyHint);
  const int page = std::max(1, params.value("page", 1));
  const int size = std::clamp(params.value("page_size", 15), 1, 50);
  const double longest = std::clamp(params.value("max_seconds", 15.0), 0.5, 600.0);
  char range[64];
  std::snprintf(range, sizeof range, "duration:[0 TO %.2f]", longest);
  const std::string filter = std::string("license:\"") + kCc0 + "\" " + range;
  const std::string url = std::string(kApi) + "search/?query=" + url_encode(*query) + "&filter=" + url_encode(filter) +
                          "&fields=id,name,duration,username,license,tags,previews,url&page=" + std::to_string(page) +
                          "&page_size=" + std::to_string(size) + "&token=" + url_encode(key);
  auto transport = cfg.transport ? cfg.transport : std::shared_ptr<net::Transport>(net::system_transport());
  ATM_TRY(json body, ask(*transport, url));
  json results = json::array();
  const fs::path dir = sounds_dir();
  if (body.contains("results") && body["results"].is_array())
    for (const json &s : body["results"]) {
      if (!s.is_object())
        continue;
      json one = sound_of(s);
      if (one["license"] != "CC0") // the filter asks for CC0 only; a sound that is not stays out
        continue;
      const fs::path file = dir / (file_stem(one["id"].get<int64_t>()) + ".mp3");
      std::error_code ec;
      one["downloaded"] = fs::exists(file, ec);
      if (one["downloaded"] == true)
        one["path"] = to_utf8(file);
      results.push_back(std::move(one));
    }
  return json{{"query", *query}, {"count", body.value("count", int64_t(0))}, {"page", page},
              {"more", body.contains("next") && body["next"].is_string()}, {"results", std::move(results)}};
}

  // sfx.fetch {id, project?, output?}: one sound downloaded (its high-quality preview, an MP3) into the sound folder with a .json of who made
  // it; with project it is imported (asset_id), with output it is also copied there. Asked again, the file on disk is used.
Result<json> Engine::Impl::sfx_fetch(const json &params) {
  ATM_PROFILE_SCOPE("api.sfx_fetch");
  const auto it = params.find("id");
  if (it == params.end() || !(it->is_number_integer() || (it->is_string() && !it->get<std::string>().empty())))
    return bad_param("id", "is required: the id of a sound from sfx.search");
  int64_t id = 0;
  if (it->is_number_integer())
    id = it->get<int64_t>();
  else if (std::sscanf(it->get<std::string>().c_str(), "%lld", reinterpret_cast<long long *>(&id)) != 1)
    return bad_param("id", "must be a number");
  if (id <= 0)
    return bad_param("id", "must be a number above 0");
  const fs::path dir = sounds_dir();
  const fs::path file = dir / (file_stem(id) + ".mp3"), about = dir / (file_stem(id) + ".json");
  std::error_code ec;
  json info;
  bool cached = fs::exists(file, ec) && fs::exists(about, ec);
  if (cached) {
    const auto text = storage::read_file(about);
    info = text ? json::parse(*text, nullptr, false) : json();
    cached = info.is_object();
  }
  if (!cached) {
    const std::string key = freesound_key();
    if (key.empty())
      return fail(ErrorCode::PolicyConsentMissing, "S_NO_KEY", "No Freesound API key has been given.", {}, kKeyHint);
    auto transport = cfg.transport ? cfg.transport : std::shared_ptr<net::Transport>(net::system_transport());
    ATM_TRY(json s, ask(*transport, std::string(kApi) + "sounds/" + std::to_string(id) +
                                        "/?fields=id,name,duration,username,license,tags,previews,url&token=" + url_encode(key)));
    info = sound_of(s);
    if (info["license"] != "CC0")
      return fail(ErrorCode::PolicyLicenseBlocked, "S_LICENSE",
                  "This sound is under " + info["license"].get<std::string>() + ", not CC0; Attome takes only CC0 sounds.", {},
                  "Pick another sound from sfx.search: it lists CC0 sounds only.");
    const std::string preview = info.value("preview", std::string());
    if (preview.rfind("https://", 0) != 0)
      return fail(ErrorCode::ProviderError, "S_NO_PREVIEW", "Freesound gave no file for this sound.");
    ATM_CHECK(storage::make_dirs(dir));
    const fs::path part = dir / (file_stem(id) + ".mp3.part");
    {
      std::ofstream out(part, std::ios::binary | std::ios::trunc);
      if (!out)
        return fail(ErrorCode::IoError, "S_FOLDER", "The file " + to_utf8(part) + " cannot be written.");
      int status = 0;
      ATM_TRY(net::Response r, transport->get(
                                   net::Request{preview},
                                   [&](const net::Response &resp) {
                                     status = resp.status;
                                     return resp.status == 200;
                                   },
                                   [&](const uint8_t *data, size_t size) {
                                     out.write(reinterpret_cast<const char *>(data), std::streamsize(size));
                                     return bool(out);
                                   }));
      (void)r;
      if (status != 200 || !out) {
        out.close();
        fs::remove(part, ec);
        return fail(ErrorCode::ProviderError, "S_HTTP", "The sound could not be downloaded (" + std::to_string(status) + ").");
      }
    }
    if (fs::file_size(part, ec) == 0 || ec) {
      fs::remove(part, ec);
      return fail(ErrorCode::ProviderError, "S_HTTP", "Freesound sent an empty file.");
    }
    fs::rename(part, file, ec);
    if (ec)
      return fail(ErrorCode::IoError, "S_FOLDER", "The file " + to_utf8(file) + " cannot be written.");
    ATM_CHECK(storage::atomic_write(about, info.dump(2) + "\n"));
  }
  json out = info;
  out["path"] = to_utf8(file);
  out["cached"] = cached;
  if (const std::string output = params.value("output", std::string()); !output.empty()) {
    fs::copy_file(file, to_path(output), fs::copy_options::overwrite_existing, ec);
    if (ec)
      return fail(ErrorCode::IoError, "S_FOLDER", "The sound cannot be copied to " + output + ".");
    out["output"] = output;
  }
  if (params.contains("project")) {
    ATM_TRY(json added, media_import({{"project", params["project"]}, {"paths", json::array({to_utf8(file)})}}));
    if (added.contains("assets") && added["assets"].is_array() && !added["assets"].empty())
      out["asset_id"] = added["assets"][0].value("id", std::string());
  }
  return out;
}

  // sfx.downloaded {}: the sounds in the sound folder, newest first, each with its credit line.
Result<json> Engine::Impl::sfx_downloaded(const json &) {
  const fs::path dir = sounds_dir();
  std::vector<std::pair<fs::file_time_type, json>> found;
  std::error_code ec;
  for (fs::directory_iterator d(dir, ec), end; !ec && d != end; d.increment(ec)) {
    const fs::path p = d->path();
    if (p.extension() != ".json" || p.stem().string().rfind("freesound_", 0) != 0)
      continue;
    const fs::path sound = dir / (p.stem().string() + ".mp3");
    if (!fs::exists(sound, ec))
      continue;
    const auto text = storage::read_file(p);
    json info = text ? json::parse(*text, nullptr, false) : json();
    if (!info.is_object())
      continue;
    info["path"] = to_utf8(sound);
    found.emplace_back(fs::last_write_time(sound, ec), std::move(info));
  }
  std::sort(found.begin(), found.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
  json list = json::array();
  for (auto &[t, info] : found)
    list.push_back(std::move(info));
  return json{{"folder", to_utf8(dir)}, {"has_key", !freesound_key().empty()}, {"sounds", std::move(list)}};
}

} // namespace atm::api
