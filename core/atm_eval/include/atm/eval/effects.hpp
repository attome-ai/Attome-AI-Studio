#pragma once
// The picture effects and the transitions of this build, with their parameters, ranges and defaults, in one table.
// The validator (atm_patch), the renderer (atm_render), the timeline ops (atm_api) and the editor all read it, so a
// new effect is added here and in the renderer, not in four places.
//
// An effect object is {"effect": "attome.<id>@1.0.0", "enabled": true, "params": {<key>: number, ...}}. The version
// suffix is optional when reading. A transition is {"type": "attome.<id>", "from", "to", "in_offset", "out_offset",
// "params": {...}}; a wipe has "params": {"direction": "left" | "right" | "up" | "down", "softness": 0.01 .. 1}.

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace atm::eval {

struct EffectParam {
  const char *key;
  const char *title;
  double lo, hi, def;
  bool required; // the validator refuses an effect that leaves it out (a blur without a radius does nothing)
  double ui_hi;  // the top of the editor's slider: a blur radius is allowed up to 0.25 but is useful up to 0.1
};

struct EffectDef {
  const char *id;    // "gaussian_blur"; the full name is "attome.gaussian_blur"
  const char *title; // for menus
  const char *alias; // another short name accepted by the timeline ops ("blur"), or ""
  std::span<const EffectParam> params;
  // The name the editor's controls use: the alias when there is one ("blur", "grade"), else the id.
  const char *short_name() const { return alias[0] != '\0' ? alias : id; }
};

std::span<const EffectDef> effect_defs();

// "attome.vignette@1.0.0", "attome.vignette", "vignette" or an alias -> the definition, or nullptr.
const EffectDef *find_effect(std::string_view name);
// "attome.<id>@1.0.0"
std::string effect_name(const EffectDef &def);
// Comma-separated ids for error hints: "gaussian_blur, color_grade, vignette".
std::string effect_ids();

enum class WipeDirection { left, right, up, down }; // the side the incoming clip enters from
struct WipeParams {
  WipeDirection direction = WipeDirection::left;
  float softness = 0.1f; // width of the soft edge, as a fraction of the picture
};
bool parse_wipe_direction(std::string_view name, WipeDirection &out);
const char *wipe_direction_name(WipeDirection d);

// The transitions of this build. A wipe and a push both take a direction (the side the incoming clip enters from); a
// wipe also takes a softness. A push slides the outgoing clip away and the incoming one in behind it. A zoom takes an
// amount: the outgoing picture grows to (1 + amount) times its size around the centre while the incoming one settles
// from that size to 1, the two cross-faded, so the camera seems to fly through one scene into the next.
enum class TransitionKind { dissolve, wipe, push, zoom };

constexpr double kZoomMin = 0.05, kZoomMax = 2.0, kZoomDefault = 0.5; // params.amount of a zoom

// "attome.dissolve", "dissolve", "attome.wipe", "wipe", "push", "zoom" (with or without "attome.") -> the id; ""
// when unknown.
std::string transition_id(std::string_view type);
bool parse_transition(std::string_view type, TransitionKind &out);
inline bool transition_has_direction(TransitionKind k) { return k == TransitionKind::wipe || k == TransitionKind::push; }
std::string transition_ids(); // "dissolve, wipe, push, zoom"

} // namespace atm::eval
