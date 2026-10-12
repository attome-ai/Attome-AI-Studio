#include "atm/gen/cast.hpp"

#include <algorithm>
#include <cctype>
#include <map>

#include "atm/base/id.hpp"

namespace atm::gen {
namespace {

std::string text_at(const json &owner, const char *key) {
  const auto it = owner.is_object() ? owner.find(key) : owner.end();
  return owner.is_object() && it != owner.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::string lowered(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return out;
}

// A value of an attribute, as the words a prompt can hold.
std::string attribute_text(const json &value) {
  if (value.is_string())
    return value.get<std::string>();
  if (value.is_boolean())
    return value.get<bool>() ? "yes" : "no";
  if (value.is_number_integer())
    return std::to_string(value.get<int64_t>());
  if (value.is_number()) {
    std::string text = std::to_string(value.get<double>());
    while (text.find('.') != std::string::npos && (text.back() == '0' || text.back() == '.')) {
      const bool dot = text.back() == '.';
      text.pop_back();
      if (dot)
        break;
    }
    return text;
  }
  return {};
}

bool is_kind(std::string_view kind) { return kind == "character" || kind == "location" || kind == "prop"; }

} // namespace

const json *find_cast(const json &cast, std::string_view id_or_name) {
  if (!cast.is_object() || id_or_name.empty())
    return nullptr;
  if (const auto it = cast.find(std::string(id_or_name)); it != cast.end() && it->is_object())
    return &*it;
  const std::string wanted = lowered(id_or_name);
  for (auto it = cast.begin(); it != cast.end(); ++it)
    if (it->is_object() && lowered(text_at(*it, "name")) == wanted)
      return &*it;
  return nullptr;
}

std::string cast_id(const json &cast, std::string_view id_or_name) {
  if (!cast.is_object() || id_or_name.empty())
    return {};
  if (const auto it = cast.find(std::string(id_or_name)); it != cast.end() && it->is_object())
    return it.key();
  const std::string wanted = lowered(id_or_name);
  for (auto it = cast.begin(); it != cast.end(); ++it)
    if (it->is_object() && lowered(text_at(*it, "name")) == wanted)
      return it.key();
  return {};
}

std::vector<std::string> cast_references(const json &entry) {
  std::vector<std::string> out;
  const auto refs = entry.is_object() ? entry.find("references") : entry.end();
  if (!entry.is_object() || refs == entry.end() || !refs->is_array())
    return out;
  for (const json &r : *refs)
    if (const std::string path = r.is_string() ? r.get<std::string>() : text_at(r, "path"); !path.empty())
      out.push_back(path);
  return out;
}

std::string cast_voice(const json &entry) {
  const auto voice = entry.is_object() ? entry.find("voice") : entry.end();
  if (!entry.is_object() || voice == entry.end())
    return {};
  return voice->is_string() ? voice->get<std::string>() : text_at(*voice, "path");
}

std::string cast_attributes_text(const json &entry) {
  const auto attributes = entry.is_object() ? entry.find("attributes") : entry.end();
  if (!entry.is_object() || attributes == entry.end() || !attributes->is_object())
    return {};
  std::string out;
  for (auto it = attributes->begin(); it != attributes->end(); ++it) { // a json object is in the order of its names
    const std::string value = attribute_text(*it);
    if (value.empty())
      continue;
    out += (out.empty() ? "" : ", ") + it.key() + ": " + value;
  }
  return out;
}

json cast_port_value(const json &entry, std::string_view port, std::string_view attribute) {
  if (!entry.is_object())
    return nullptr;
  if (port == "description" || port == "personality") {
    const std::string text = text_at(entry, port == "description" ? "description" : "personality");
    return text.empty() ? json(nullptr) : json(text);
  }
  if (port == "references") {
    const std::vector<std::string> pictures = cast_references(entry);
    return pictures.empty() ? json(nullptr) : json(pictures);
  }
  if (port == "voice") {
    const std::string voice = cast_voice(entry);
    return voice.empty() ? json(nullptr) : json(voice);
  }
  if (port == "attributes") {
    const std::string text = cast_attributes_text(entry);
    return text.empty() ? json(nullptr) : json(text);
  }
  if (port == "attribute") {
    const auto attributes = entry.find("attributes");
    if (attribute.empty() || attributes == entry.end() || !attributes->is_object())
      return nullptr;
    const auto it = attributes->find(std::string(attribute));
    if (it == attributes->end())
      return nullptr;
    const std::string text = attribute_text(*it);
    return text.empty() ? json(nullptr) : json(text);
  }
  return nullptr;
}

bool cast_text(const json &cast, std::string_view token, std::string &out) {
  if (!cast.is_object() || token.empty())
    return false;
  if (const json *whole = find_cast(cast, token)) { // the whole token is a name (a name may hold a dot)
    const std::string name = text_at(*whole, "name"), look = text_at(*whole, "description");
    out = look.empty() ? name : name + ", " + look;
    return !out.empty();
  }
  const size_t dot = token.rfind('.');
  if (dot == std::string_view::npos || dot == 0 || dot + 1 >= token.size())
    return false;
  const json *entry = find_cast(cast, token.substr(0, dot));
  if (!entry)
    return false;
  const std::string_view part = token.substr(dot + 1);
  const json value = part == "description" || part == "personality" || part == "attributes" ? cast_port_value(*entry, part, "")
                                                                                             : cast_port_value(*entry, "attribute", part);
  if (!value.is_string())
    return false;
  out = value.get<std::string>();
  return true;
}

void check_cast(const json &cast, std::vector<Problem> &out) {
  if (!cast.is_object())
    return;
  std::map<std::string, std::string> names; // lowered name -> the first entry that has it
  for (auto it = cast.begin(); it != cast.end(); ++it) {
    const auto add = [&](const std::string &field, std::string message, std::string hint) {
      out.push_back(Problem{"G_CAST", it.key() + (field.empty() ? "" : "/" + field), it.key(), std::move(message), std::move(hint)});
    };
    if (!it->is_object()) {
      add("", "The cast entry " + it.key() + " must be an object.", "Give it a name, a kind and a description.");
      continue;
    }
    const std::string name = text_at(*it, "name");
    if (name.empty())
      add("name", "The cast entry " + it.key() + " has no name.", "Give it a name: it is how clips and agents ask for it.");
    else if (const auto [first, fresh] = names.emplace(lowered(name), it.key()); !fresh)
      add("name", "Two cast entries are named \"" + name + "\": " + first->second + " and " + it.key() + ".", "Give each its own name.");
    if (const std::string kind = text_at(*it, "kind"); !is_kind(kind))
      add("kind", "The cast entry " + it.key() + " has the kind \"" + kind + "\".", "Use character, location or prop.");
    if (const auto refs = it->find("references"); refs != it->end()) {
      if (!refs->is_array())
        add("references", "The references of " + it.key() + " must be a list.", "Give a list of {path, label}.");
      else
        for (size_t n = 0; n < refs->size(); ++n)
          if (const json &r = (*refs)[n]; (r.is_string() ? r.get<std::string>() : text_at(r, "path")).empty())
            add("references/" + std::to_string(n), "Reference " + std::to_string(n + 1) + " of " + it.key() + " has no path.", "Give the path of a picture file.");
    }
    if (const auto voice = it->find("voice"); voice != it->end() && !voice->is_null() && cast_voice(*it).empty())
      add("voice", "The voice of " + it.key() + " has no path.", "Give the path of a sound file, or take the voice away.");
    if (const auto attributes = it->find("attributes"); attributes != it->end()) {
      if (!attributes->is_object())
        add("attributes", "The attributes of " + it.key() + " must be an object of names and values.", "Write {\"alive\": false, \"age\": 34}.");
      else
        for (auto a = attributes->begin(); a != attributes->end(); ++a)
          if (!a->is_string() && !a->is_boolean() && !a->is_number())
            add("attributes/" + a.key(), "The attribute \"" + a.key() + "\" of " + it.key() + " must be a text, a number or yes/no.",
                "Keep one plain value for each attribute.");
    }
  }
}

bool use_character(json &workflow, std::string_view character_id, bool voice) {
  if (!workflow.is_object() || !workflow.contains("nodes") || !workflow["nodes"].is_object() || !workflow.contains("exposed") || character_id.empty())
    return false;
  json &nodes = workflow["nodes"];
  std::string gen;
  int characters = 0;
  for (auto it = nodes.begin(); it != nodes.end(); ++it) {
    const std::string kind = it->value("kind", std::string());
    if (kind == "attome.generate_video")
      gen = it.key();
    else if (kind == "attome.character")
      ++characters;
  }
  json &inputs = workflow["exposed"]["inputs"];
  if (gen.empty() || !inputs.is_object() || !inputs.contains("references"))
    return false;
  const bool placeholders = gen.rfind("$new:", 0) == 0;
  const std::string n = std::to_string(characters + 1);
  const std::string node = placeholders ? "$new:character" + n : new_id("nod");
  nodes[node] = {{"kind", "attome.character"}, {"character", std::string(character_id)}, {"ui", {{"x", -520}, {"y", 560 + 140 * characters}}}};
  json &links = workflow["links"];
  links[placeholders ? "$new:l_character" + n : new_id("lnk")] = {{"from", json::array({node, "references"})}, {"to", json::array({gen, "references"})}};
  inputs["references"].erase("to"); // an input is either exposed or linked: the pictures now come from the cast
  if (voice && inputs.contains("voice")) {
    links[placeholders ? "$new:l_character_voice" + n : new_id("lnk")] = {{"from", json::array({node, "voice"})}, {"to", json::array({gen, "voice"})}};
    inputs["voice"].erase("to"); // an input is either exposed or linked: the voices now come from the cast
  }
  return true;
}

std::vector<std::string> reference_order(const std::vector<std::string> &pictures) {
  std::vector<std::string> out = pictures;
  std::sort(out.begin(), out.end(), [](const std::string &a, const std::string &b) { return json(a).dump() < json(b).dump(); });
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

} // namespace atm::gen
