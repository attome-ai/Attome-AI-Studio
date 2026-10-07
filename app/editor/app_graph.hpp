#pragma once
// Drawing helpers of the workflow graph, shared by the workflow canvas and the generative clip's card.

#include "app_support.hpp"

namespace atm::editor {


constexpr float kNodeW = 236.0f, kNodeTitleH = 46.0f, kPortRowH = 24.0f, kPortR = 5.5f;

inline ImU32 port_colour(gen::PortType type, int alpha = 255) {
  switch (type) {
  case gen::PortType::text: return IM_COL32(210, 216, 230, alpha);
  case gen::PortType::number:
  case gen::PortType::integer: return IM_COL32(240, 200, 90, alpha);
  case gen::PortType::boolean: return IM_COL32(240, 140, 170, alpha);
  case gen::PortType::image: return IM_COL32(110, 210, 130, alpha);
  case gen::PortType::video: return IM_COL32(90, 140, 240, alpha);
  case gen::PortType::audio: return IM_COL32(60, 190, 170, alpha);
  case gen::PortType::mask: return IM_COL32(200, 200, 200, alpha);
  case gen::PortType::conditioning: return IM_COL32(255, 150, 80, alpha);
  case gen::PortType::latent: return IM_COL32(170, 120, 250, alpha);
  }
  return IM_COL32(200, 200, 200, alpha);
}

constexpr float kPreviewH = 96.0f; // the picture of a node's last result under its ports

inline float node_height(const gen::Ports &ports) {
  return kNodeTitleH + float(std::max<size_t>(1, std::max(ports.inputs.size(), ports.outputs.size()))) * kPortRowH + 10.0f;
}


// ["nod_...", "port"] -> the two strings.
inline bool end_of(const json &pair, std::string &node, std::string &port) {
  if (!pair.is_array() || pair.size() != 2 || !pair[0].is_string() || !pair[1].is_string())
    return false;
  node = pair[0].get<std::string>();
  port = pair[1].get<std::string>();
  return true;
}

// "Encode prompt" for "attome.encode_prompt"; the name itself for a kind this build does not know.
inline std::string kind_title(const std::string &kind) {
  if (gen::is_workflow_kind(kind))
    return "Workflow";
  const gen::KindDef *def = gen::find_kind(kind);
  return def ? def->title : kind;
}

inline std::string kind_id(const std::string &kind) {
  const gen::KindDef *def = gen::find_kind(kind);
  return def ? def->id : kind;
}

// The distance from p to the curve of a link, sampled: enough to pick one with the mouse.
inline float curve_distance(ImVec2 p, ImVec2 a, ImVec2 b) {
  const float bend = std::max(40.0f, std::fabs(b.x - a.x) * 0.5f);
  const ImVec2 c1(a.x + bend, a.y), c2(b.x - bend, b.y);
  float best = 1e9f;
  for (int i = 0; i <= 24; ++i) {
    const float t = float(i) / 24.0f, u = 1.0f - t;
    const float x = u * u * u * a.x + 3 * u * u * t * c1.x + 3 * u * t * t * c2.x + t * t * t * b.x;
    const float y = u * u * u * a.y + 3 * u * u * t * c1.y + 3 * u * t * t * c2.y + t * t * t * b.y;
    best = std::min(best, std::hypot(p.x - x, p.y - y));
  }
  return best;
}

inline void draw_link(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 colour, float width) {
  const float bend = std::max(40.0f, std::fabs(b.x - a.x) * 0.5f);
  dl->AddBezierCubic(a, ImVec2(a.x + bend, a.y), ImVec2(b.x - bend, b.y), b, colour, width);
}



} // namespace atm::editor
