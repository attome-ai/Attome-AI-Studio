#include "atm/gen/library.hpp"

#include <algorithm>
#include <map>

#include "atm/base/id.hpp"

namespace atm::gen {
namespace {

constexpr std::string_view kShotPrefix = "shot:";

json exposed_input(const char *type, const char *label, int order, const char *node, const char *port, bool required = false) {
  json e = {{"type", type}, {"label", label}, {"order", order}, {"to", json::array({json::array({node, port})})}};
  if (required)
    e["required"] = true;
  return e;
}

} // namespace

std::vector<std::string> builtin_workflow_ids() {
  std::vector<std::string> out;
  for (const std::string &id : model_ids())
    if (const ModelDecl *m = find_model(id); m && m->does("generate_video"))
      out.push_back(std::string(kShotPrefix) + id);
  return out;
}

json shot_workflow(const ModelDecl &model) {
  json settings = json::object();
  for (const SettingDecl &s : model.settings)
    if (!s.def.is_null())
      settings[s.name] = s.def;
  const char *gen = "$new:gen";
  json inputs = json::object();
  int order = 0;
  inputs["prompt"] = exposed_input("text", "Prompt", order++, gen, "prompt", true);
  for (const char *name : {"start_image", "end_image", "references"})
    if (model.takes(name)) {
      const std::string label = std::string(name) == "start_image" ? "Start image" : std::string(name) == "end_image" ? "End image" : "References";
      inputs[name] = exposed_input("image", label.c_str(), order++, gen, name);
    }
  inputs["seed"] = exposed_input("integer", "Seed", order++, gen, "seed");

  json node = {{"kind", "attome.generate_video"}, {"model", model.id}};
  if (!settings.empty())
    node["settings"] = std::move(settings);
  // The size comes from the project (about 0.9 megapixels in the canvas's shape, as a model makes best), the length from
  // the clip: nothing about the timeline is held in the workflow.
  const auto link = [](const char *from, const char *from_port, const char *to, const char *to_port) {
    return json{{"from", json::array({from, from_port})}, {"to", json::array({to, to_port})}};
  };
  json nodes = {{gen, std::move(node)},
                {"$new:wf_project", {{"kind", "attome.project"}, {"settings", {{"pixels", kGenerationPixels}}}, {"ui", {{"x", -260}, {"y", 260}}}}},
                {"$new:wf_clip", {{"kind", "attome.clip"}, {"ui", {{"x", -260}, {"y", 420}}}}}};
  json links = {{"$new:l_width", link("$new:wf_project", "width", gen, "width")},
                {"$new:l_height", link("$new:wf_project", "height", gen, "height")},
                {"$new:l_seconds", link("$new:wf_clip", "duration", gen, "seconds")}};
  return {{"name", "Shot"},
          {"source", std::string(kShotPrefix) + model.id},
          {"nodes", std::move(nodes)},
          {"links", std::move(links)},
          {"exposed",
           {{"inputs", std::move(inputs)},
            {"outputs", {{"video", {{"from", {gen, "video"}}}}, {"audio", {{"from", {gen, "audio"}}}}}},
            {"primary", "video"}}}};
}

bool start_from(json &workflow, std::string_view reference) {
  if (!workflow.is_object() || !workflow.contains("nodes") || !workflow["nodes"].is_object() || !workflow.contains("exposed"))
    return false;
  json &nodes = workflow["nodes"];
  std::string gen;
  for (auto it = nodes.begin(); it != nodes.end(); ++it)
    if (const std::string kind = it->value("kind", std::string()); kind == "attome.generate_video" || kind == "attome.sample")
      gen = it.key();
  json &inputs = workflow["exposed"]["inputs"];
  if (gen.empty() || !inputs.contains("start_image"))
    return false;
  const bool placeholders = gen.rfind("$new:", 0) == 0;
  const std::string ref = placeholders ? "$new:reference" : new_id("nod"), frame = placeholders ? "$new:frame" : new_id("nod");
  nodes[ref] = {{"kind", "attome.clip_reference"}, {"settings", {{"clip", std::string(reference)}}}, {"ui", {{"x", -520}, {"y", 20}}}};
  nodes[frame] = {{"kind", "attome.get_frame"}, {"settings", {{"frame", "last"}}}, {"ui", {{"x", -260}, {"y", 20}}}};
  json &links = workflow["links"];
  links[placeholders ? "$new:l_ref" : new_id("lnk")] = {{"from", json::array({ref, "video"})}, {"to", json::array({frame, "video"})}};
  links[placeholders ? "$new:l_frame" : new_id("lnk")] = {{"from", json::array({frame, "image"})}, {"to", json::array({gen, "start_image"})}};
  inputs["start_image"].erase("to");
  return true;
}

json fresh_copy(const json &workflow, std::string_view source) {
  std::map<std::string, std::string> node_ids, link_ids;
  json out = {{"name", workflow.value("name", std::string("Workflow"))}, {"nodes", json::object()}, {"links", json::object()}};
  if (!source.empty())
    out["source"] = std::string(source);
  int n = 0;
  if (workflow.contains("nodes") && workflow["nodes"].is_object())
    for (auto it = workflow["nodes"].begin(); it != workflow["nodes"].end(); ++it) {
      node_ids[it.key()] = "$new:n" + std::to_string(++n);
      out["nodes"][node_ids[it.key()]] = *it;
    }
  const auto end_of = [&](const json &pair) -> json {
    if (!pair.is_array() || pair.size() != 2 || !pair[0].is_string())
      return pair;
    const auto id = node_ids.find(pair[0].get<std::string>());
    return id == node_ids.end() ? pair : json::array({id->second, pair[1]});
  };
  int l = 0;
  if (workflow.contains("links") && workflow["links"].is_object())
    for (auto it = workflow["links"].begin(); it != workflow["links"].end(); ++it) {
      json link = *it;
      if (link.is_object()) {
        if (link.contains("from"))
          link["from"] = end_of(link["from"]);
        if (link.contains("to"))
          link["to"] = end_of(link["to"]);
      }
      out["links"]["$new:l" + std::to_string(++l)] = std::move(link);
    }
  if (workflow.contains("exposed") && workflow["exposed"].is_object()) {
    json exposed = workflow["exposed"];
    if (exposed.contains("inputs") && exposed["inputs"].is_object())
      for (auto it = exposed["inputs"].begin(); it != exposed["inputs"].end(); ++it)
        if (it->is_object() && it->contains("to") && (*it)["to"].is_array())
          for (json &pair : (*it)["to"])
            pair = end_of(pair);
    if (exposed.contains("outputs") && exposed["outputs"].is_object())
      for (auto it = exposed["outputs"].begin(); it != exposed["outputs"].end(); ++it)
        if (it->is_object() && it->contains("from"))
          (*it)["from"] = end_of((*it)["from"]);
    out["exposed"] = std::move(exposed);
  }
  return out;
}

const ModelDecl *main_model(const json &workflow) {
  const ModelDecl *found = nullptr;
  if (workflow.is_object() && workflow.contains("nodes") && workflow["nodes"].is_object())
    for (auto it = workflow["nodes"].begin(); it != workflow["nodes"].end(); ++it) {
      const std::string kind = it->value("kind", std::string()), model = it->value("model", std::string());
      if (const ModelDecl *m = model.empty() ? nullptr : find_model(model))
        if (m->does("generate_video") || m->does("sample"))
          found = found ? found : m;
    }
  return found;
}

json instantiate(std::string_view source_id) {
  if (source_id.substr(0, kShotPrefix.size()) == kShotPrefix)
    if (const ModelDecl *m = find_model(source_id.substr(kShotPrefix.size())); m && m->does("generate_video"))
      return shot_workflow(*m);
  return nullptr;
}

} // namespace atm::gen
