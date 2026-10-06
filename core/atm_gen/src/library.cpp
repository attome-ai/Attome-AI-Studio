#include "atm/gen/library.hpp"

#include <algorithm>
#include <map>
#include <tuple>

#include "atm/base/id.hpp"

namespace atm::gen {
namespace {

constexpr std::string_view kShotPrefix = "shot:";
constexpr std::string_view kVoicePrefix = "voice:";

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
  for (const std::string &id : model_ids())
    if (const ModelDecl *m = find_model(id); m && m->does("generate_speech"))
      out.push_back(std::string(kVoicePrefix) + id);
  std::sort(out.begin(), out.end());
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
  // Frames and notes of the canvas come along, with their own IDs.
  for (const char *deco : {"groups", "notes"})
    if (workflow.contains(deco) && workflow[deco].is_object()) {
      out[deco] = json::object();
      int d = 0;
      for (auto it = workflow[deco].begin(); it != workflow[deco].end(); ++it)
        out[deco]["$new:" + std::string(deco) + std::to_string(++d)] = *it;
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

bool is_comfy_graph(const json &graph) {
  if (!graph.is_object() || graph.empty())
    return false;
  for (auto it = graph.begin(); it != graph.end(); ++it)
    if (!it->is_object() || !it->contains("class_type") || !(*it)["class_type"].is_string())
      return false;
  return true;
}

json from_comfy(const json &graph, std::vector<std::string> &unmatched, std::string_view name) {
  if (!is_comfy_graph(graph))
    return nullptr;
  const auto kind_of = [](const std::string &cls) -> const char * {
    if (cls == "CLIPTextEncode")
      return "attome.encode_prompt";
    if (cls == "KSampler" || cls == "KSamplerAdvanced" || cls == "SamplerCustomAdvanced")
      return "attome.sample";
    if (cls == "VAEDecode" || cls == "VAEDecodeTiled")
      return "attome.decode";
    return nullptr;
  };
  json nodes = json::object(), links = json::object(), exposed_in = json::object(), exposed_out = json::object();
  std::map<std::string, std::string> placed; // ComfyUI id -> "$new:…"
  std::string first_encode, first_sample, first_decode;
  int n = 0, l = 0;
  for (auto it = graph.begin(); it != graph.end(); ++it) {
    const std::string cls = (*it)["class_type"].get<std::string>();
    const char *kind = kind_of(cls);
    if (!kind) {
      const std::string title = it->contains("_meta") && (*it)["_meta"].is_object() ? (*it)["_meta"].value("title", std::string()) : std::string();
      unmatched.push_back(cls + (title.empty() || title == cls ? "" : " \"" + title + "\"") + " (node " + it.key() + ")");
      continue;
    }
    const std::string id = "$new:c" + std::to_string(++n);
    placed[it.key()] = id;
    json node = {{"kind", kind}};
    const json inputs = it->value("inputs", json::object());
    json typed = json::object();
    if (std::string_view(kind) == "attome.encode_prompt") {
      if (first_encode.empty())
        first_encode = id;
      if (inputs.contains("text") && inputs["text"].is_string())
        typed["prompt"] = inputs["text"];
    } else if (std::string_view(kind) == "attome.sample") {
      if (first_sample.empty())
        first_sample = id;
      for (const char *seed : {"seed", "noise_seed"})
        if (inputs.contains(seed) && inputs[seed].is_number_integer())
          typed["seed"] = inputs[seed];
    } else if (first_decode.empty()) {
      first_decode = id;
    }
    if (!typed.empty())
      node["inputs"] = std::move(typed);
    nodes[id] = std::move(node);
  }
  // Links between the nodes that came: ComfyUI writes one as [source id, output slot].
  for (auto it = graph.begin(); it != graph.end(); ++it) {
    const auto to = placed.find(it.key());
    if (to == placed.end())
      continue;
    const json inputs = it->value("inputs", json::object());
    const std::string kind = nodes[to->second].value("kind", std::string());
    for (const auto &[input, port, from_kind, from_port] :
         {std::tuple<const char *, const char *, const char *, const char *>{"positive", "conditioning", "attome.encode_prompt", "conditioning"},
          {"conditioning", "conditioning", "attome.encode_prompt", "conditioning"},
          {"samples", "latent", "attome.sample", "latent"}}) {
      if (!inputs.contains(input) || !inputs[input].is_array() || inputs[input].size() != 2 || !inputs[input][0].is_string())
        continue;
      const auto from = placed.find(inputs[input][0].get<std::string>());
      if (from == placed.end() || nodes[from->second].value("kind", std::string()) != from_kind)
        continue;
      if ((std::string_view(port) == "conditioning" && kind != "attome.sample") || (std::string_view(port) == "latent" && kind != "attome.decode"))
        continue;
      links["$new:l" + std::to_string(++l)] = {{"from", json::array({from->second, from_port})}, {"to", json::array({to->second, port})}};
    }
  }
  int order = 0;
  if (!first_encode.empty()) {
    json prompt = {{"type", "text"}, {"label", "Prompt"}, {"order", order++}, {"required", true}, {"to", json::array({json::array({first_encode, "prompt"})})}};
    if (nodes[first_encode].contains("inputs") && nodes[first_encode]["inputs"].contains("prompt")) {
      prompt["default"] = nodes[first_encode]["inputs"]["prompt"];
      nodes[first_encode]["inputs"].erase("prompt");
      if (nodes[first_encode]["inputs"].empty())
        nodes[first_encode].erase("inputs");
    }
    exposed_in["prompt"] = std::move(prompt);
  }
  if (!first_sample.empty()) {
    json seed = {{"type", "integer"}, {"label", "Seed"}, {"order", order++}, {"to", json::array({json::array({first_sample, "seed"})})}};
    if (nodes[first_sample].contains("inputs") && nodes[first_sample]["inputs"].contains("seed")) {
      seed["default"] = nodes[first_sample]["inputs"]["seed"];
      nodes[first_sample]["inputs"].erase("seed");
      if (nodes[first_sample]["inputs"].empty())
        nodes[first_sample].erase("inputs");
    }
    exposed_in["seed"] = std::move(seed);
  }
  json out = {{"name", std::string(name.empty() ? "ComfyUI workflow" : name)}, {"nodes", std::move(nodes)}, {"links", std::move(links)}};
  json exposed = {{"inputs", std::move(exposed_in)}};
  if (!first_decode.empty()) {
    exposed_out["video"] = {{"from", json::array({first_decode, "video"})}};
    exposed["primary"] = "video";
  }
  exposed["outputs"] = std::move(exposed_out);
  out["exposed"] = std::move(exposed);
  return out;
}

const ModelDecl *main_model(const json &workflow) {
  const ModelDecl *found = nullptr;
  if (workflow.is_object() && workflow.contains("nodes") && workflow["nodes"].is_object())
    for (auto it = workflow["nodes"].begin(); it != workflow["nodes"].end(); ++it) {
      const std::string kind = it->value("kind", std::string()), model = it->value("model", std::string());
      if (const ModelDecl *m = model.empty() ? nullptr : find_model(model))
        if (m->does("generate_video") || m->does("sample") || m->does("generate_speech"))
          found = found ? found : m;
    }
  return found;
}

json voice_workflow(const ModelDecl &model) {
  json settings = json::object();
  for (const SettingDecl &s : model.settings)
    if (!s.def.is_null())
      settings[s.name] = s.def;
  const char *say = "$new:say";
  json node = {{"kind", "attome.generate_speech"}, {"model", model.id}};
  if (!settings.empty())
    node["settings"] = std::move(settings);
  json inputs = json::object();
  inputs["text"] = exposed_input("text", "Text", 0, say, "text", true);
  inputs["voice"] = exposed_input("text", "Voice", 1, say, "instruct");
  inputs["voice"]["default"] = "female, young adult, moderate pitch";
  inputs["seed"] = exposed_input("integer", "Seed", 2, say, "seed");
  return {{"name", "Voice"},
          {"source", std::string(kVoicePrefix) + model.id},
          {"nodes", {{say, std::move(node)}, {"$new:len", {{"kind", "attome.get_duration"}, {"ui", {{"x", 260}, {"y", 200}}}}}}},
          {"links", {{"$new:l_len", {{"from", json::array({say, "audio"})}, {"to", json::array({"$new:len", "audio"})}}}}},
          {"exposed",
           {{"inputs", std::move(inputs)},
            {"outputs", {{"audio", {{"from", {say, "audio"}}}}, {"length", {{"from", {"$new:len", "seconds"}}}}}},
            {"primary", "audio"}}}};
}

json instantiate(std::string_view source_id) {
  if (source_id.substr(0, kVoicePrefix.size()) == kVoicePrefix)
    if (const ModelDecl *m = find_model(source_id.substr(kVoicePrefix.size())); m && m->does("generate_speech"))
      return voice_workflow(*m);
  if (source_id.substr(0, kShotPrefix.size()) == kShotPrefix)
    if (const ModelDecl *m = find_model(source_id.substr(kShotPrefix.size())); m && m->does("generate_video"))
      return shot_workflow(*m);
  return nullptr;
}

} // namespace atm::gen
