#include "atm/api/script_plan.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <sstream>

#include "atm/base/profiler.hpp"

namespace atm::api::script {

namespace {

int count_words(const std::string &text) {
  std::istringstream in(text);
  std::string w;
  int n = 0;
  while (in >> w)
    ++n;
  return n;
}

std::string lower_letters(const std::string &word) { // "Gross." -> "gross", "twenty-four" stays
  std::string out;
  for (const unsigned char c : word)
    if (std::isalpha(c) || c == '-')
      out += char(std::tolower(c));
  return out;
}

double round_to(double v, double step) { return std::round(v / step) * step; }

} // namespace

json plan(const json &spec) {
  ATM_PROFILE_SCOPE("script.plan");
  json warnings = json::array();
  const auto warn = [&](const std::string &where, const std::string &what) { warnings.push_back({{"where", where}, {"message", what}}); };
  json out = {{"ok", true}};
  const double wps_low = spec.value("words_per_second_min", 1.8), wps_high = spec.value("words_per_second_max", 3.6);
  const double wps_pace = spec.value("words_per_second", 2.6); // how fast the narrator speaks, to give a scene its length
  const double lead = spec.value("voice_lead", 0.1);           // the voice starts a little after the picture does
  const double tail = spec.value("tail", 0.0);

  const json scenes_in = spec.value("scenes", json::array());
  if (!scenes_in.is_array() || scenes_in.empty()) {
    out["ok"] = false;
    out["error"] = "spec needs \"scenes\": a list of {say, label?, seconds?, prompt?}";
    return out;
  }
  std::vector<std::string> keys;
  if (spec.contains("key_words") && spec["key_words"].is_array())
    for (const json &k : spec["key_words"])
      if (k.is_string())
        keys.push_back(lower_letters(k.get<std::string>()));

  json scenes = json::array();
  double clock = 0.0;
  int total_words = 0;
  std::vector<std::string> spoken;
  std::set<std::string> needed;
  for (size_t i = 0; i < scenes_in.size(); ++i) {
    const json &s = scenes_in[i];
    const std::string where = "scene " + std::to_string(i + 1);
    if (!s.is_object() || !s.contains("say") || !s["say"].is_string() || count_words(s["say"].get<std::string>()) == 0) {
      out["ok"] = false;
      out["error"] = where + " needs \"say\": the words the narrator says in it";
      return out;
    }
    const std::string say = s["say"].get<std::string>();
    const int words = count_words(say);
    total_words += words;
    // Its length: asked for, or the time the words take at the narrator's pace (a half second more), in half seconds, 3 s at the least.
    double seconds = s.contains("seconds") && s["seconds"].is_number() ? s["seconds"].get<double>() : std::max(3.0, round_to(double(words) / wps_pace + 0.5, 0.5));
    if (seconds <= 0.0) {
      out["ok"] = false;
      out["error"] = where + ": \"seconds\" must be above 0";
      return out;
    }
    const double start = s.contains("start") && s["start"].is_number() ? s["start"].get<double>() : clock;
    if (start + 1e-6 < clock)
      warn(where, "starts at " + std::to_string(start) + " s, before the scene before it has ended (" + std::to_string(clock) + " s)");
    const double rate = double(words) / std::max(0.1, seconds - lead);
    if (rate > wps_high)
      warn(where, std::to_string(words) + " words in " + std::to_string(seconds).substr(0, 4) + " s is " + std::to_string(rate).substr(0, 3) +
                      " words a second: too fast to follow; shorten the line or give the scene more seconds");
    else if (rate < wps_low)
      warn(where, "only " + std::to_string(rate).substr(0, 3) + " words a second: the scene will feel empty; add words or shorten the scene");
    const char last = say.back();
    if (last != '.' && last != '!' && last != '?')
      warn(where, "the line does not end on . ! or ?: captions and pauses are cut at punctuation");
    json found = json::array();
    std::istringstream in(say);
    std::string w;
    while (in >> w) {
      const std::string bare = lower_letters(w);
      if (std::find(keys.begin(), keys.end(), bare) != keys.end() && std::find(found.begin(), found.end(), bare) == found.end())
        found.push_back(bare);
      spoken.push_back(bare);
    }
    json scene = {{"n", i + 1}, {"label", s.value("label", json())}, {"say", say}, {"words", words}, {"start", start}, {"end", start + seconds},
                  {"seconds", seconds}, {"voice_at", start + lead}, {"words_per_second", std::round(rate * 100.0) / 100.0}, {"key_words", found}};
    if (s.contains("prompt") && s["prompt"].is_string()) {
      const std::string prompt = s["prompt"].get<std::string>();
      scene["prompt"] = prompt;
      for (const char *var : {"character", "style"})
        if (prompt.find(std::string("{") + var + "}") != std::string::npos)
          needed.insert(var);
    }
    scenes.push_back(std::move(scene));
    clock = start + seconds;
  }
  const double total = clock + tail;
  if (spec.contains("target_seconds") && spec["target_seconds"].is_array() && spec["target_seconds"].size() == 2) { // a Short: [30, 40]
    const double lo = spec["target_seconds"][0].get<double>(), hi = spec["target_seconds"][1].get<double>();
    if (total < lo || total > hi)
      warn("film", "it is " + std::to_string(total).substr(0, std::to_string(total).find('.') + 2) + " s long; " + std::to_string(int(lo)) + " to " + std::to_string(int(hi)) + " s was asked for");
  }
  if (spec.contains("target_words") && spec["target_words"].is_array() && spec["target_words"].size() == 2) {
    const int lo = spec["target_words"][0].get<int>(), hi = spec["target_words"][1].get<int>();
    if (total_words < lo || total_words > hi)
      warn("film", std::to_string(total_words) + " words in the narration; " + std::to_string(lo) + " to " + std::to_string(hi) + " fits the length");
  }
  if (spec.contains("hook_max_seconds") && spec["hook_max_seconds"].is_number() && scenes.front().value("seconds", 0.0) > spec["hook_max_seconds"].get<double>())
    warn("scene 1", "the opening should be over in " + std::to_string(spec["hook_max_seconds"].get<double>()).substr(0, 3) + " s: the first seconds decide whether anyone stays");
  for (const std::string &k : keys)
    if (std::find(spoken.begin(), spoken.end(), k) == spoken.end())
      warn("key_words", "\"" + k + "\" is never said: it will not be shown in colour");
  // Variables: {character} and {style} in the prompts are values the scenes share; they have to exist, with a value, before the scenes are made.
  json variables = json::array();
  for (const std::string &v : needed) {
    const bool given = spec.contains(v) && spec[v].is_string() && !spec[v].get<std::string>().empty();
    variables.push_back({{"name", v}, {"value", given ? json(spec[v]) : json(nullptr)}});
    if (!given)
      warn("variables", "the prompts use {" + v + "} but the spec gives no \"" + v + "\"");
  }
  out["scenes"] = std::move(scenes);
  out["total_seconds"] = total;
  out["total_words"] = total_words;
  out["cuts"] = [&] {
    json cuts = json::array();
    for (size_t i = 1; i < out["scenes"].size(); ++i)
      cuts.push_back(out["scenes"][i]["start"]);
    return cuts;
  }();
  out["variables"] = std::move(variables);
  out["warnings"] = std::move(warnings);
  return out;
}

json text_ops(const json &plan, const json &look) {
  ATM_PROFILE_SCOPE("script.text_ops");
  if (!plan.is_object() || !plan.value("ok", false) || !plan.contains("scenes") || !plan["scenes"].is_array() || plan["scenes"].empty())
    return {{"ok", false}, {"error", "plan must be the answer of script.plan (with its scenes)"}};
  const json cap = look.value("captions", json::object()), lab = look.value("labels", json::object()), hook = look.value("hook", json::object()),
             cta = look.value("cta", json::object());
  const double lead = cap.value("lead", 0.12), tail = cap.value("tail", 0.12);
  json ops = json::array();
  int captions = 0, labels = 0;
  const auto add_style = [](json &op, const json &extra) {
    if (extra.is_object())
      for (auto it = extra.begin(); it != extra.end(); ++it)
        op[it.key()] = it.value();
  };
  const auto seconds_text = [](double s) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3fs", s);
    return std::string(buf);
  };
  const bool has_hook = hook.contains("text") && hook["text"].is_string() && !hook["text"].get<std::string>().empty();
  const bool has_cta = cta.contains("text") && cta["text"].is_string() && !cta["text"].get<std::string>().empty();
  if (has_hook || has_cta) // over the captions and labels, on a track of their own: they may share a moment with a label
    ops.push_back({{"op", "add_track"}, {"id", "$new:hooktrack"}, {"kind", "video"}, {"name", look.value("hook_track", std::string("Hook"))}});
  double total = 0.0;
  for (const json &scene : plan["scenes"]) {
    total = std::max(total, scene.value("end", 0.0));
    const double start = scene.value("start", 0.0), end = scene.value("end", start);
    if (scene.contains("say") && scene["say"].is_string()) {
      json op = {{"op", "add_captions"}, {"id", "$new:cap" + std::to_string(captions)}, {"style", cap.value("style", std::string("pop"))}};
      if (scene.contains("voice_clip") && scene["voice_clip"].is_string()) {
        op["clip"] = scene["voice_clip"];
      } else {
        op["text"] = scene["say"];
        op["at"] = seconds_text(start + lead);
        op["duration"] = seconds_text(std::max(0.2, end - start - lead - tail));
      }
      for (const char *k : {"size", "y", "color", "emphasis_color", "track"})
        if (cap.contains(k))
          op[k] = cap[k];
      if (scene.contains("key_words") && scene["key_words"].is_array() && !scene["key_words"].empty())
        op["emphasis"] = scene["key_words"];
      ops.push_back(std::move(op));
      ++captions;
    }
    if (scene.contains("label") && scene["label"].is_string() && !scene["label"].get<std::string>().empty()) {
      json op = {{"op", "add_text"}, {"text", scene["label"]}, {"name", "Label"}, {"at", seconds_text(start + lab.value("offset", 0.1))},
                 {"duration", seconds_text(lab.value("seconds", 1.5))}, {"size", lab.value("size", 0.055)},
                 {"position", json::array({0.5, lab.value("y", 0.14)})}, {"color", lab.value("color", std::string("#FFE600"))}};
      add_style(op, lab.value("extra", json::object()));
      ops.push_back(std::move(op));
      ++labels;
    }
  }
  if (has_hook) {
    json op = {{"op", "add_text"}, {"text", hook["text"]}, {"name", "Hook"}, {"at", "0s"}, {"duration", seconds_text(hook.value("seconds", 3.0))},
               {"size", hook.value("size", 0.075)}, {"position", json::array({0.5, hook.value("y", 0.27)})}, {"color", hook.value("color", std::string("#FFFFFF"))},
               {"track", "$new:hooktrack"}};
    add_style(op, hook.value("extra", json::object()));
    ops.push_back(std::move(op));
  }
  if (has_cta) {
    const double secs = cta.value("seconds", 2.0);
    json op = {{"op", "add_text"}, {"text", cta["text"]}, {"name", "Call to action"}, {"at", seconds_text(cta.value("at", std::max(0.0, total - secs)))},
               {"duration", seconds_text(secs)}, {"size", cta.value("size", 0.075)}, {"position", json::array({0.5, cta.value("y", 0.4)})},
               {"color", cta.value("color", std::string("#FFE600"))}, {"track", "$new:hooktrack"}};
    add_style(op, cta.value("extra", json::object()));
    ops.push_back(std::move(op));
  }
  return {{"ok", true}, {"ops", std::move(ops)}, {"captions", captions}, {"labels", labels}};
}

} // namespace atm::api::script
