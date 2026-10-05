#include "atm/gen/library.hpp"

#include <algorithm>

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
  json seconds = exposed_input("number", "Length", order++, gen, "seconds");
  seconds["range"] = {{"min", model.seconds_min > 0.0 ? model.seconds_min : 1.0}, {"max", model.seconds_max > 0.0 ? model.seconds_max : 15.0}};
  inputs["seconds"] = std::move(seconds);
  inputs["width"] = exposed_input("integer", "Width", order++, gen, "width");
  inputs["height"] = exposed_input("integer", "Height", order++, gen, "height");

  json node = {{"kind", "attome.generate_video"}, {"model", model.id}};
  if (!settings.empty())
    node["settings"] = std::move(settings);
  return {{"name", "Shot"},
          {"source", std::string(kShotPrefix) + model.id},
          {"nodes", {{gen, std::move(node)}}},
          {"links", json::object()},
          {"exposed",
           {{"inputs", std::move(inputs)},
            {"outputs", {{"video", {{"from", {gen, "video"}}}}, {"audio", {{"from", {gen, "audio"}}}}, {"last_frame", {{"from", {gen, "last_frame"}}}}}},
            {"primary", "video"}}}};
}

json instantiate(std::string_view source_id) {
  if (source_id.substr(0, kShotPrefix.size()) == kShotPrefix)
    if (const ModelDecl *m = find_model(source_id.substr(kShotPrefix.size())); m && m->does("generate_video"))
      return shot_workflow(*m);
  return nullptr;
}

} // namespace atm::gen
