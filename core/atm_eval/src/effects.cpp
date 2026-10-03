#include "atm/eval/effects.hpp"

#include <array>

namespace atm::eval {
namespace {

constexpr EffectParam kBlur[] = {
    {"radius", "Radius", 0.0, 0.25, 0.02, true, 0.1},
};
constexpr EffectParam kGrade[] = {
    {"brightness", "Brightness", -1.0, 1.0, 0.0, false, 1.0},
    {"contrast", "Contrast", -1.0, 1.0, 0.0, false, 1.0},
    {"saturation", "Saturation", 0.0, 3.0, 1.0, false, 3.0},
};
constexpr EffectParam kVignette[] = {
    {"strength", "Strength", 0.0, 1.0, 0.5, false, 1.0},
    {"radius", "Radius", 0.0, 1.0, 0.55, false, 1.0},
    {"softness", "Softness", 0.01, 1.0, 0.45, false, 1.0},
};

constexpr EffectDef kEffects[] = {
    {"gaussian_blur", "Blur", "blur", kBlur},
    {"color_grade", "Color grade", "grade", kGrade},
    {"vignette", "Vignette", "", kVignette},
};

constexpr std::array<std::pair<const char *, WipeDirection>, 4> kDirections = {{
    {"left", WipeDirection::left}, {"right", WipeDirection::right}, {"up", WipeDirection::up}, {"down", WipeDirection::down}}};

std::string_view strip(std::string_view name) {
  if (const auto at = name.find('@'); at != std::string_view::npos)
    name = name.substr(0, at);
  constexpr std::string_view kPrefix = "attome.";
  if (name.substr(0, kPrefix.size()) == kPrefix)
    name = name.substr(kPrefix.size());
  return name;
}

} // namespace

std::span<const EffectDef> effect_defs() { return kEffects; }

const EffectDef *find_effect(std::string_view name) {
  name = strip(name);
  for (const EffectDef &d : kEffects)
    if (name == d.id || (d.alias[0] != '\0' && name == d.alias))
      return &d;
  return nullptr;
}

std::string effect_name(const EffectDef &def) { return std::string("attome.") + def.id + "@1.0.0"; }

std::string effect_ids() {
  std::string out;
  for (const EffectDef &d : kEffects)
    out += (out.empty() ? "" : ", ") + std::string(d.id);
  return out;
}

bool parse_wipe_direction(std::string_view name, WipeDirection &out) {
  for (const auto &[n, d] : kDirections)
    if (name == n) {
      out = d;
      return true;
    }
  return false;
}

const char *wipe_direction_name(WipeDirection d) {
  for (const auto &[n, dir] : kDirections)
    if (dir == d)
      return n;
  return "left";
}

std::string transition_id(std::string_view type) {
  type = strip(type);
  return type == "dissolve" || type == "wipe" || type == "push" || type == "zoom" ? std::string(type) : std::string();
}

bool parse_transition(std::string_view type, TransitionKind &out) {
  const std::string id = transition_id(type);
  if (id.empty())
    return false;
  out = id == "wipe"   ? TransitionKind::wipe
        : id == "push" ? TransitionKind::push
        : id == "zoom" ? TransitionKind::zoom
                       : TransitionKind::dissolve;
  return true;
}

std::string transition_ids() { return "dissolve, wipe, push, zoom"; }

} // namespace atm::eval
