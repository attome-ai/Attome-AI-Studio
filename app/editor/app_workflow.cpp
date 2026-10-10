// The workflow editor: node graph canvas, copy and paste of nodes, the side panel.
#include "app_support.hpp"
#include "app_graph.hpp"
#include <algorithm>
#include <span>
#include <chrono>
#include <cmath>
#include <numeric>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>
#include <imgui.h>
#include <imgui_internal.h>
#include <random>
#include "atm/base/id.hpp"
#include "atm/base/profiler.hpp"
#include "atm/base/time.hpp"
#include "atm/gen/graph.hpp"
#include "atm/gen/keys.hpp"
#include "atm/gen/library.hpp"
#include "uidriver.hpp"

namespace atm::editor {

void App::poll_gen_parts() {
  if (!wf_parts_.is_null() && clock_ < next_wf_parts_poll_)
    return;
  next_wf_parts_poll_ = clock_ + 3.0;
  json parts;
  RpcError error;
  if (client_.call("gen.nodes", json::object(), parts, error))
    wf_parts_ = std::move(parts);
  else if (wf_parts_.is_null())
    wf_parts_ = json::object();
}

const json *App::clip_json(const std::string &clip_id) const {
  const std::function<const json *(const json &)> walk = [&](const json &node) -> const json * {
    if (!node.is_object())
      return nullptr;
    if (const auto clips = node.find("clips"); clips != node.end() && clips->is_object())
      if (const auto c = clips->find(clip_id); c != clips->end())
        return &*c;
    for (const char *key : {"sequences", "tracks"})
      if (const auto inner = node.find(key); inner != node.end() && inner->is_object())
        for (const json &child : *inner)
          if (const json *found = walk(child))
            return found;
    return nullptr;
  };
  return walk(doc_);
}

// The open workflow: a clip's own (its Instance), or an entry of the project's library.
const json *App::workflow_json() const {
  if (wf_id_.empty())
    return nullptr;
  if (id_prefix(wf_id_) == "clp") {
    const json *clip = clip_json(wf_id_);
    if (!clip)
      return nullptr;
    const json &instance = object_in(object_in(*clip, "media_ref"), "workflow");
    return instance.empty() ? nullptr : &instance;
  }
  const json &library = object_in(doc_, "workflows");
  const auto it = library.find(wf_id_);
  return it != library.end() && it->is_object() ? &*it : nullptr;
}

// The path to the open workflow in a patch.
std::string App::wf_base() const { return id_prefix(wf_id_) == "clp" ? wf_id_ + "/media_ref/workflow" : wf_id_; }

void App::open_workflow(const std::string &target) {
  play(false);
  mode_ = 1;
  wf_id_ = target;
  wf_clip_ = id_prefix(target) == "clp" ? target : std::string();
  wf_node_.clear();
  wf_link_.clear();
  wf_row_.clear();
  wf_sel_.clear();
  wf_deco_.clear();
  wf_results_clip_.clear();
  wf_search_.open = false;
  wf_moved_.clear();
  wf_drag_ = {};
  wf_fit_ = true; // the whole graph in view
  wf_fit_all_ = std::getenv("ATTOME_EDITOR_SCRIPT") != nullptr && std::getenv("ATTOME_UI_READABLE_FIT") == nullptr; // a UI test script sees the whole graph; a person gets a size text can be read at
}

void App::draw_workflows() {
  const ImGuiViewport *vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, hexv(look::panel));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
  ImGui::Begin("##workflows", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking);
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();

  const json &library = object_in(doc_, "workflows");
  if (!workflow_json()) { // the open one is gone (undo, or none was open): the first clip's, else the first of the library
    wf_id_.clear();
    for (const TrackUi &t : tracks_)
      for (const ClipUi &c : t.clips)
        if (wf_id_.empty() && c.is_generative)
          wf_id_ = c.id;
    if (wf_id_.empty() && !library.empty())
      wf_id_ = library.begin().key();
    wf_clip_ = id_prefix(wf_id_) == "clp" ? wf_id_ : std::string();
    wf_node_.clear();
    wf_link_.clear();
  }
  poll_gen_parts();

  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const float left_w = 252.0f, right_w = 340.0f;
  const auto panel = [&](const char *id, float width, const std::function<void()> &body) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, hexv(look::panel));
    if (ImGui::BeginChild(id, ImVec2(width, avail.y), ImGuiChildFlags_AlwaysUseWindowPadding))
      body();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
  };
  panel("##wf_left", left_w, [&] { draw_workflow_list(library); });
  ImGui::SameLine(0.0f, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, hexv(look::bg));
  if (ImGui::BeginChild("##wf_canvas", ImVec2(std::max(120.0f, avail.x - left_w - right_w), avail.y), ImGuiChildFlags_None,
                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
    draw_workflow_canvas(library);
  ImGui::EndChild();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
  ImGui::SameLine(0.0f, 0.0f);
  panel("##wf_side", right_w, [&] { draw_workflow_side(library); });
  ImGui::End();
}

// The left column: back to the timeline, the workflows of the project (each generative clip's own, and the library's), and
// the node kinds to add.
// "res_multistep" -> "Res multistep": a setting's name as a label.
static std::string pretty_name(std::string name) {
  std::replace(name.begin(), name.end(), '_', ' ');
  if (!name.empty())
    name[0] = char(std::toupper(static_cast<unsigned char>(name[0])));
  return name;
}

// "shot:minimax-h3.fl2va.turbo8-int8" -> "Shot, MiniMax H3: ...": where a workflow came from, in words (the id stays in the tooltip).
static std::string readable_source(const json &models, const std::string &source) {
  for (const char *prefix : {"shot:", "voice:"}) {
    const std::string p = prefix;
    if (source.rfind(p, 0) != 0)
      continue;
    const std::string id = source.substr(p.size());
    std::string kind = p.substr(0, p.size() - 1);
    kind[0] = char(std::toupper(static_cast<unsigned char>(kind[0])));
    for (const json &m : models)
      if (m.value("id", std::string()) == id) {
        std::string title = m.value("title", id);
        if (const size_t colon = title.find(':'); colon != std::string::npos)
          title = title.substr(0, colon);
        return kind + ", " + title;
      }
    return kind + ", " + id;
  }
  return source;
}

void App::draw_workflow_list(const json &library) {
  if (!gen_models_loaded_) { // the models' titles, to say where a workflow came from in words
    gen_models_loaded_ = true;
    json listed;
    if (rpc("gen.models", json::object(), listed))
      gen_models_ = listed.value("models", json::array());
  }
  if (soft_button("wf_back", "<  Back to the timeline", ImVec2(-1.0f, 30.0f)))
    mode_ = 0;
  ImGui::Dummy(ImVec2(0.0f, 10.0f));
  ImGui::PushFont(g_fonts.bold, 15.0f);
  ImGui::TextUnformatted("Workflows");
  ImGui::PopFont();
  ImGui::Dummy(ImVec2(0.0f, 4.0f));
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextColored(hexv(look::fg3), "Every generative clip has its own copy of the workflow it was made from. Change one and no other clip changes.");
  ImGui::PopTextWrapPos();
  ImGui::Dummy(ImVec2(0.0f, 8.0f));
  // One row: its name, what it is, and a click opens it.
  const auto row = [&](const std::string &id, const std::string &name, const std::string &note) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(("##wf_" + id).c_str(), ImVec2(-1.0f, 44.0f));
    ui_mark("workflow:" + name);
    ui_mark("workflow:" + id);
    const bool hovered = ImGui::IsItemHovered(), open = id == wf_id_;
    if (ImGui::IsItemClicked() && !open)
      open_workflow(id);
    const ImVec2 q(p.x + ImGui::GetItemRectSize().x, p.y + 44.0f);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, q, hex(open ? look::raised : hovered ? look::panel2 : look::bg), 9.0f);
    if (open)
      dl->AddRectFilled(p, ImVec2(p.x + 3.0f, q.y), hex(look::accent), 2.0f);
    dl->PushClipRect(p, ImVec2(q.x - 8.0f, q.y), true);
    dl->AddText(ImVec2(p.x + 12.0f, p.y + 6.0f), hex(look::fg), name.c_str());
    dl->AddText(ImVec2(p.x + 12.0f, p.y + 24.0f), hex(look::fg3), note.c_str());
    dl->PopClipRect();
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
  };
  bool any_clip = false;
  for (const TrackUi &t : tracks_)
    for (const ClipUi &c : t.clips)
      if (c.is_generative) {
        if (!any_clip)
          section_label("CLIPS");
        if (!any_clip)
          ImGui::Dummy(ImVec2(0.0f, 4.0f));
        any_clip = true;
        row(c.id, c.name, c.source.empty() ? "its own workflow" : "from " + readable_source(gen_models_, c.source));
      }
  if (!library.empty()) {
    if (any_clip)
      ImGui::Dummy(ImVec2(0.0f, 6.0f));
    section_label("LIBRARY");
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    for (auto it = library.begin(); it != library.end(); ++it)
      row(it.key(), it->value("name", std::string("Workflow")), "in the project's library");
  }
  if (soft_button("wf_new", "+  New workflow", ImVec2(-1.0f, 30.0f)))
    pending_ = [this] {
      json ids;
      if (patch(json::array({{{"op", "add"},
                              {"path", project_id_ + "/workflows/$new:w"},
                              {"value", {{"name", "Workflow"}, {"nodes", json::object()}, {"links", json::object()},
                                         {"exposed", {{"inputs", json::object()}, {"outputs", json::object()}}}}}}}),
                "New workflow", &ids))
        open_workflow(ids.value("$new:w", ""));
    };

  ImGui::Dummy(ImVec2(0.0f, 14.0f));
  section_label("ADD A NODE");
  ImGui::Dummy(ImVec2(0.0f, 6.0f));
  const json kinds = wf_parts_.value("kinds", json::array());
  for (const json &k : kinds) {
    const std::string id = k.value("id", ""), kind = k.value("kind", ""), title = k.value("title", id);
    if (!soft_button(("wf_add_" + id).c_str(), title.c_str(), ImVec2(-1.0f, 30.0f), workflow_json() != nullptr))
      continue;
    // It has no place of its own yet: the graph lays it out by what it is linked to, until it is dragged somewhere. It
    // takes the model of a node that is there when that model runs this kind too, with the model's own defaults: the
    // usual case is one model for the whole workflow.
    json value = new_node_value(id);
    pending_ = [this, value, title] {
      json ids;
      if (patch(json::array({{{"op", "add"}, {"path", wf_base() + "/nodes/$new:n"}, {"value", value}}}), ("Add " + title).c_str(), &ids)) {
        wf_node_ = ids.value("$new:n", "");
        wf_link_.clear();
      }
    };
  }
  // A workflow of the library as one node (a Subgraph): its Exposed Inputs are the node's inputs, its Outputs the node's. A workflow is
  // not offered inside itself.
  {
    std::vector<std::pair<std::string, std::string>> usable;
    for (auto w = library.begin(); w != library.end(); ++w)
      if (w.key() != wf_id_)
        usable.emplace_back(w.key(), w->value("name", w.key()));
    if (!usable.empty() && workflow_json()) {
      ImGui::Dummy(ImVec2(0.0f, 10.0f));
      section_label("A WORKFLOW AS A NODE");
      ImGui::Dummy(ImVec2(0.0f, 6.0f));
      for (const auto &[wid, wname] : usable)
        if (soft_button(("wf_sub_" + wname).c_str(), wname.c_str(), ImVec2(-1.0f, 30.0f)))
          pending_ = [this, wid, wname] {
            json ids;
            if (patch(json::array({{{"op", "add"}, {"path", wf_base() + "/nodes/$new:n"}, {"value", {{"kind", "attome.workflow"}, {"workflow", wid}}}}}), ("Add " + wname).c_str(), &ids)) {
              wf_node_ = ids.value("$new:n", "");
              wf_sel_ = {wf_node_};
            }
          };
    }
  }
  ImGui::Dummy(ImVec2(0.0f, 10.0f));
  section_label("ON THE CANVAS");
  ImGui::Dummy(ImVec2(0.0f, 6.0f));
  if (soft_button("wf_add_group", "A frame around nodes", ImVec2(-1.0f, 30.0f), workflow_json() != nullptr))
    pending_ = [this] { add_deco(true); };
  if (soft_button("wf_add_note", "A note", ImVec2(-1.0f, 30.0f), workflow_json() != nullptr))
    pending_ = [this] { add_deco(false); };
  ImGui::Dummy(ImVec2(0.0f, 10.0f));
  section_label("FILES");
  ImGui::Dummy(ImVec2(0.0f, 6.0f));
  if (soft_button("wf_import", "Import a workflow...", ImVec2(-1.0f, 30.0f)))
    pending_ = [this] { ask_workflow_import(); };
  ui_mark("button:wf_import");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("An Attome workflow file, or a ComfyUI workflow saved as API format.");
  if (soft_button("wf_export", "Export this workflow...", ImVec2(-1.0f, 30.0f), workflow_json() != nullptr))
    pending_ = [this] { ask_workflow_export(wf_id_); };
  if (!wf_import_note_.empty()) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(hexv(look::fg2), "%s", wf_import_note_.c_str());
    ImGui::PopTextWrapPos();
  }
  ImGui::Dummy(ImVec2(0.0f, 10.0f));
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextColored(hexv(look::fg3), "Drag from a port to a port of the same colour to join them. Drag a joined port away and let go on nothing to break "
                                      "the connection. The clip's side is wired the same way. Drag a node to move it, the background to look "
                                      "around; Delete removes what is selected.");
  ImGui::PopTextWrapPos();
}

// A node of a kind as it is first put in the workflow: the model of a node that is there when that model runs this kind too, with the
// model's own defaults (the usual case is one model for the whole workflow); the Input nodes start with what makes them useful.
json App::new_node_value(const std::string &kind_id) const {
  const gen::KindDef *def = gen::find_kind(kind_id);
  json value = {{"kind", def ? gen::kind_name(*def) : kind_id}};
  if (def && std::string_view(def->id) == "clip_reference")
    value["settings"] = {{"clip", "previous"}};
  const json &nodes = object_in(workflow_json() ? *workflow_json() : json::object(), "nodes");
  for (auto n = nodes.begin(); n != nodes.end() && !value.contains("model"); ++n)
    for (const json &m : wf_parts_.value("models", json::array()))
      if (m.value("id", "") == n->value("model", std::string("?")) && std::find(m["kinds"].begin(), m["kinds"].end(), kind_id) != m["kinds"].end()) {
        value["model"] = m["id"];
        json settings = json::object();
        for (const json &s : m.value("settings", json::array()))
          if (!s["default"].is_null())
            settings[s.value("name", "")] = s["default"];
        if (!settings.empty())
          value["settings"] = std::move(settings);
      }
  return value;
}

// A frame or a note. A frame goes around the nodes that are selected (its title bar above them); with none selected, and for a note, it
// goes in the free space under the graph.
void App::add_deco(bool group) {
  const json *workflow = workflow_json();
  if (!workflow)
    return;
  const json &nodes = object_in(*workflow, "nodes");
  std::set<std::string> chosen = wf_sel_;
  if (!wf_node_.empty())
    chosen.insert(wf_node_);
  float lo_x = 1e9f, lo_y = 1e9f, hi_x = -1e9f, hi_y = -1e9f, all_lo_x = 1e9f, all_hi_y = -1e9f;
  for (auto n = nodes.begin(); n != nodes.end(); ++n) {
    const auto at = wf_pos_.find(n.key());
    if (at == wf_pos_.end())
      continue;
    const float h = node_height(gen::node_ports(object_in(doc_, "workflows"), *n)) + (wf_results_.contains(n.key()) ? kPreviewH : 0.0f);
    all_lo_x = std::min(all_lo_x, at->second.x);
    all_hi_y = std::max(all_hi_y, at->second.y + h);
    if (chosen.contains(n.key())) {
      lo_x = std::min(lo_x, at->second.x);
      lo_y = std::min(lo_y, at->second.y);
      hi_x = std::max(hi_x, at->second.x + kNodeW);
      hi_y = std::max(hi_y, at->second.y + h);
    }
  }
  json value;
  if (group && lo_x < 1e8f)
    value = {{"title", "Group"}, {"x", std::round(lo_x - 20.0f)}, {"y", std::round(lo_y - 44.0f)}, {"w", std::round(hi_x - lo_x + 40.0f)}, {"h", std::round(hi_y - lo_y + 64.0f)}, {"color", "#4a90d9"}};
  else {
    const float x = all_lo_x < 1e8f ? all_lo_x : 300.0f, y = all_hi_y > -1e8f ? all_hi_y + 50.0f : 60.0f;
    value = group ? json{{"title", "Group"}, {"x", std::round(x)}, {"y", std::round(y)}, {"w", 360}, {"h", 220}, {"color", "#4a90d9"}}
                  : json{{"text", "Note"}, {"x", std::round(x)}, {"y", std::round(y)}};
  }
  json ids;
  if (patch(json::array({{{"op", "add"}, {"path", wf_base() + (group ? "/groups/$new:d" : "/notes/$new:d")}, {"value", value}}}), group ? "Add frame" : "Add note", &ids)) {
    wf_deco_ = ids.value("$new:d", "");
    wf_node_.clear();
    wf_sel_.clear();
    wf_row_.clear();
    wf_link_.clear();
  }
}

// The edits that take a node away: the node, the links at its ports, what the Exposed Inputs feed of it (they stay, and feed
// what they still feed) and the Outputs that came from it (the Primary Output moves to another when it was one of them).
json App::remove_node_ops(const json &workflow, const std::string &node_id) const { return remove_nodes_ops(workflow, {node_id}); }

// The same for several nodes at once: a link between two of them goes once, an Exposed Input keeps what it feeds elsewhere.
json App::remove_nodes_ops(const json &workflow, const std::set<std::string> &ids) const {
  json ops = json::array();
  const std::string base = wf_base();
  std::string node, port, other, other_port;
  const json &links = object_in(workflow, "links");
  for (auto l = links.begin(); l != links.end(); ++l)
    if ((l->contains("from") && end_of((*l)["from"], node, port) && ids.contains(node)) ||
        (l->contains("to") && end_of((*l)["to"], other, other_port) && ids.contains(other)))
      ops.push_back({{"op", "remove"}, {"path", l.key()}});
  for (const gen::ExposedInput &e : gen::exposed_inputs(object_in(doc_, "workflows"), workflow)) {
    json kept = json::array();
    for (const auto &[n, p] : e.to)
      if (!ids.contains(n))
        kept.push_back(json::array({n, p}));
    if (kept.size() != e.to.size())
      ops.push_back({{"op", "replace"}, {"path", base + "/exposed/inputs/" + e.name + "/to"}, {"value", std::move(kept)}});
  }
  std::vector<std::string> outputs_after;
  bool changed = false;
  for (const gen::ExposedOutput &o : gen::exposed_outputs(workflow)) {
    if (ids.contains(o.node)) {
      ops.push_back({{"op", "remove"}, {"path", base + "/exposed/outputs/" + o.name}});
      changed = true;
    } else {
      outputs_after.push_back(o.name);
    }
  }
  if (changed)
    for (json &op : primary_ops(outputs_after, gen::primary_output(workflow)))
      ops.push_back(std::move(op));
  for (const std::string &id : ids)
    ops.push_back({{"op", "remove"}, {"path", id}});
  return ops;
}

// Ctrl+C: the selected nodes, with where they are and the links among them (a link to a node that is not copied is left behind).
// Which node the keys choose: from the selected one (else from the first, the leftmost), by direction or in reading order.
void App::wf_key_select(int how) {
  if (how == 4 || how == 5) {
    wf_key_tab(how == 4);
    return;
  }
  if (wf_pos_.empty())
    return;
  constexpr float kW = 230.0f; // a node's width in canvas units, near enough for choosing between nodes (kNodeW is drawn at the zoom)
  struct N { std::string id; ImVec2 centre; };
  std::vector<N> all;
  for (const auto &[id, pos] : wf_pos_) {
    const auto h = wf_height_.find(id);
    all.push_back({id, ImVec2(pos.x + kW * 0.5f, pos.y + (h != wf_height_.end() ? h->second : 80.0f) * 0.5f)});
  }
  std::sort(all.begin(), all.end(), [](const N &a, const N &b) { return a.centre.x != b.centre.x ? a.centre.x < b.centre.x : a.centre.y < b.centre.y; }); // reading order: the graph flows to the right
  size_t at = all.size();
  for (size_t i = 0; i < all.size(); ++i)
    if (all[i].id == wf_node_ || (wf_node_.empty() && wf_sel_.size() == 1 && all[i].id == *wf_sel_.begin()))
      at = i;
  size_t pick = all.size();
  if (how == 6 || (at == all.size() && how != 7)) {
    pick = 0;
  } else if (how == 7) {
    pick = all.size() - 1;
  } else if (how == 4) {
    pick = (at + 1) % all.size();
  } else if (how == 5) {
    pick = (at + all.size() - 1) % all.size();
  } else {
    float best = 1e30f; // the nearest in that direction: far across counts for more than far along
    const ImVec2 from = all[at].centre;
    for (size_t i = 0; i < all.size(); ++i) {
      if (i == at)
        continue;
      const float dx = all[i].centre.x - from.x, dy = all[i].centre.y - from.y;
      const float along = how == 0 ? -dx : how == 1 ? dx : how == 2 ? -dy : dy, across = (how <= 1) ? std::fabs(dy) : std::fabs(dx);
      if (along <= 4.0f)
        continue;
      const float score = along + 2.0f * across;
      if (score < best) {
        best = score;
        pick = i;
      }
    }
  }
  if (pick >= all.size())
    return; // nothing in that direction: the selection stays
  wf_sel_ = {all[pick].id};
  wf_node_ = all[pick].id;
  wf_link_.clear();
  wf_row_.clear();
  wf_deco_.clear();
  wf_reveal_ = all[pick].id;
}

void App::wf_copy() {
  const json *workflow = workflow_json();
  std::set<std::string> ids = wf_sel_;
  if (!wf_node_.empty())
    ids.insert(wf_node_);
  if (!workflow || ids.empty())
    return;
  json copied = {{"nodes", json::object()}, {"links", json::object()}};
  const json &nodes = object_in(*workflow, "nodes"), &links = object_in(*workflow, "links");
  for (const std::string &id : ids)
    if (nodes.contains(id)) {
      json node = nodes[id];
      if (const auto at = wf_pos_.find(id); at != wf_pos_.end())
        node["ui"] = {{"x", std::round(at->second.x)}, {"y", std::round(at->second.y)}};
      copied["nodes"][id] = std::move(node);
    }
  std::string from_node, from_port, to_node, to_port;
  for (auto l = links.begin(); l != links.end(); ++l)
    if (l->contains("from") && l->contains("to") && end_of((*l)["from"], from_node, from_port) && end_of((*l)["to"], to_node, to_port) &&
        ids.contains(from_node) && ids.contains(to_node))
      copied["links"][l.key()] = *l;
  wf_clipboard_ = std::move(copied);
  wf_pasted_ = 0;
  say(std::to_string(wf_clipboard_["nodes"].size()) + (wf_clipboard_["nodes"].size() == 1 ? " node" : " nodes") + " copied");
}

// Ctrl+V and Ctrl+D: the copied nodes put into the open workflow, `offset` canvas units down and right of where they were, one step
// further for each paste in a row. The links among them come with them: one edit, one undo.
void App::wf_paste(float offset) {
  const json *workflow = workflow_json();
  if (!workflow || !wf_clipboard_.is_object() || !wf_clipboard_.contains("nodes") || wf_clipboard_["nodes"].empty())
    return;
  json copy = gen::fresh_copy(wf_clipboard_, std::string());
  ++wf_pasted_;
  const float shift = offset * float(wf_pasted_);
  const std::string base = wf_base();
  json ops = json::array();
  std::vector<std::string> placeholders;
  for (auto n = copy["nodes"].begin(); n != copy["nodes"].end(); ++n) {
    json node = *n;
    if (node.contains("ui") && node["ui"].is_object())
      node["ui"] = {{"x", node["ui"].value("x", 0.0) + double(shift)}, {"y", node["ui"].value("y", 0.0) + double(shift)}};
    placeholders.push_back(n.key());
    ops.push_back({{"op", "add"}, {"path", base + "/nodes/" + n.key()}, {"value", std::move(node)}});
  }
  for (auto l = copy["links"].begin(); l != copy["links"].end(); ++l)
    ops.push_back({{"op", "add"}, {"path", base + "/links/" + l.key()}, {"value", *l}});
  json ids;
  if (!patch(std::move(ops), "Paste nodes", &ids))
    return;
  wf_sel_.clear();
  for (const std::string &ph : placeholders)
    if (ids.contains(ph))
      wf_sel_.insert(ids[ph].get<std::string>());
  wf_node_ = wf_sel_.size() == 1 ? *wf_sel_.begin() : std::string();
  wf_link_.clear();
  wf_row_.clear();
}

// A node chosen in the search box: put where the box was opened; when the box was opened by a link let go on nothing, it is linked
// to the node that link came from.
void App::wf_add_from_search(const std::string &kind_id) {
  json value = new_node_value(kind_id);
  value["ui"] = {{"x", std::round(wf_search_.canvas.x)}, {"y", std::round(wf_search_.canvas.y)}};
  const std::string base = wf_base();
  json ops = json::array({{{"op", "add"}, {"path", base + "/nodes/$new:n"}, {"value", value}}});
  if (wf_search_.linked) {
    const gen::Ports mine = gen::node_ports(object_in(doc_, "workflows"), value);
    const gen::Port held{wf_search_.port, gen::PortType(wf_search_.type), false, false};
    if (wf_search_.from_output) { // the held output feeds an input of the new node: a required one first
      const gen::Port *pick = nullptr;
      for (const gen::Port &in : mine.inputs)
        if (gen::can_link(held, in) && (!pick || (in.required && !pick->required)))
          pick = &in;
      if (pick)
        ops.push_back({{"op", "add"}, {"path", base + "/links/$new:l"}, {"value", {{"from", {wf_search_.node, wf_search_.port}}, {"to", {"$new:n", pick->name}}}}});
    } else { // the held input is fed by an output of the new node
      const gen::Port *pick = nullptr;
      for (const gen::Port &out : mine.outputs)
        if (gen::can_link(out, held) && !pick)
          pick = &out;
      if (pick)
        ops.push_back({{"op", "add"}, {"path", base + "/links/$new:l"}, {"value", {{"from", {"$new:n", pick->name}}, {"to", {wf_search_.node, wf_search_.port}}}}});
    }
  }
  json ids;
  wf_search_.open = false;
  if (patch(std::move(ops), "Add node", &ids)) {
    wf_node_ = ids.value("$new:n", "");
    wf_sel_ = {wf_node_};
    wf_link_.clear();
    wf_row_.clear();
    wf_reveal_ = wf_node_;
  }
}

// Tab and Shift+Tab: what can be chosen, one after another: the nodes in reading order, then the frames, the notes, the inputs of the clip
// and its outputs (the rows of the two boxes at the sides). The side panel then shows what was chosen.
void App::wf_key_tab(bool next) {
  const json *workflow = workflow_json();
  if (!workflow)
    return;
  struct Target {
    int kind; // 0 node, 1 frame, 2 note, 3 row
    std::string id, words;
  };
  std::vector<Target> all;
  std::vector<std::pair<ImVec2, std::string>> nodes;
  for (const auto &[id, pos] : wf_pos_)
    nodes.emplace_back(pos, id);
  std::sort(nodes.begin(), nodes.end(), [](const auto &a, const auto &b) { return a.first.x != b.first.x ? a.first.x < b.first.x : a.first.y < b.first.y; });
  for (const auto &[pos, id] : nodes)
    all.push_back({0, id, ""});
  const json &groups = object_in(*workflow, "groups"), &notes = object_in(*workflow, "notes");
  for (auto g = groups.begin(); g != groups.end(); ++g)
    all.push_back({1, g.key(), "Frame \"" + g->value("title", std::string("Group")) + "\""});
  for (auto n = notes.begin(); n != notes.end(); ++n)
    all.push_back({2, n.key(), "Note"});
  const json &library = object_in(doc_, "workflows");
  for (const gen::ExposedInput &e : gen::exposed_inputs(library, *workflow))
    all.push_back({3, "in:" + e.name, "Input of the clip: " + e.name});
  for (const gen::ExposedOutput &o : gen::exposed_outputs(*workflow))
    all.push_back({3, "out:" + o.name, "Output of the clip: " + o.name});
  if (all.empty())
    return;
  size_t at = all.size();
  for (size_t i = 0; i < all.size(); ++i) {
    const Target &t = all[i];
    if ((t.kind == 0 && (t.id == wf_node_ || (wf_node_.empty() && wf_sel_.size() == 1 && t.id == *wf_sel_.begin()))) || (t.kind <= 2 && t.kind >= 1 && t.id == wf_deco_) ||
        (t.kind == 3 && t.id == wf_row_))
      at = i;
  }
  const size_t pick = at == all.size() ? (next ? 0 : all.size() - 1) : (next ? (at + 1) % all.size() : (at + all.size() - 1) % all.size());
  const Target &t = all[pick];
  wf_sel_.clear();
  wf_node_.clear();
  wf_deco_.clear();
  wf_row_.clear();
  wf_link_.clear();
  if (t.kind == 0) {
    wf_sel_ = {t.id};
    wf_node_ = t.id;
  } else if (t.kind == 3) {
    wf_row_ = t.id;
  } else {
    wf_deco_ = t.id;
  }
  wf_reveal_ = t.id;
  if (!t.words.empty())
    say(t.words);
}

// The links that touch the chosen node, one after another: the selected link is the one Delete removes, as when a link is clicked.
void App::wf_key_link(int dir) {
  const json *workflow = workflow_json();
  if (!workflow || wf_node_.empty())
    return;
  const json &links = object_in(*workflow, "links");
  std::vector<std::string> mine;
  std::map<std::string, std::string> words;
  for (auto l = links.begin(); l != links.end(); ++l) {
    if (!l->is_object() || !l->contains("from") || !l->contains("to") || !(*l)["from"].is_array() || !(*l)["to"].is_array() || (*l)["from"].size() < 2 || (*l)["to"].size() < 2)
      continue;
    const json &from = (*l)["from"], &to = (*l)["to"];
    if (from[0].get<std::string>() != wf_node_ && to[0].get<std::string>() != wf_node_)
      continue;
    mine.push_back(l.key());
    words[l.key()] = from[1].get<std::string>() + " to " + to[1].get<std::string>();
  }
  if (mine.empty()) {
    say("This node has no links.");
    return;
  }
  const auto at = std::find(mine.begin(), mine.end(), wf_link_);
  size_t pick = at == mine.end() ? (dir > 0 ? 0 : mine.size() - 1) : (size_t(std::ptrdiff_t(at - mine.begin()) + dir + std::ptrdiff_t(mine.size())) % mine.size());
  wf_link_ = mine[pick];
  wf_row_.clear();
  say("Link " + std::to_string(pick + 1) + " of " + std::to_string(mine.size()) + ": " + words[wf_link_] + ". Delete removes it; [ and ] go on.");
}

// The chosen nodes moved by a step, as one saved edit (the same op as a drag that ends).
void App::wf_nudge(float dx, float dy) {
  const json *workflow = workflow_json();
  if (!workflow)
    return;
  const json &nodes = object_in(*workflow, "nodes");
  if (!wf_deco_.empty()) { // a frame or a note: it moves (a frame takes the nodes whose middle is inside it, as a drag does)
    const json &groups = object_in(*workflow, "groups"), &notes = object_in(*workflow, "notes");
    const bool is_group = groups.contains(wf_deco_);
    if (!is_group && !notes.contains(wf_deco_))
      return;
    const json &was = is_group ? groups[wf_deco_] : notes[wf_deco_];
    const float x = was.value("x", 0.0f), y = was.value("y", 0.0f), w = was.value("w", 320.0f), h = was.value("h", 200.0f);
    json ops = json::array();
    const auto set = [&](const char *field, double value) {
      ops.push_back({{"op", was.contains(field) ? "replace" : "add"}, {"path", wf_deco_ + "/" + field}, {"value", value}});
    };
    set("x", std::round(x + dx));
    set("y", std::round(y + dy));
    if (is_group)
      for (const auto &[id, pos] : wf_pos_) {
        const auto node = nodes.find(id);
        const auto height = wf_height_.find(id);
        const ImVec2 mid(pos.x + kNodeW * 0.5f, pos.y + (height != wf_height_.end() ? height->second : 80.0f) * 0.5f);
        if (node != nodes.end() && mid.x >= x && mid.x <= x + w && mid.y >= y && mid.y <= y + h)
          ops.push_back({{"op", node->contains("ui") ? "replace" : "add"}, {"path", id + "/ui"}, {"value", {{"x", std::round(pos.x + dx)}, {"y", std::round(pos.y + dy)}}}});
      }
    patch(std::move(ops), ops.size() > 2 ? "Move frame" : "Move");
    wf_reveal_ = wf_deco_;
    return;
  }
  if (wf_sel_.empty())
    return;
  json ops = json::array();
  for (const std::string &id : wf_sel_) {
    const auto node = nodes.find(id);
    const auto at = wf_pos_.find(id);
    if (node == nodes.end() || at == wf_pos_.end())
      continue;
    ops.push_back({{"op", node->contains("ui") ? "replace" : "add"}, {"path", id + "/ui"},
                   {"value", {{"x", std::round(at->second.x + dx)}, {"y", std::round(at->second.y + dy)}}}});
  }
  if (!ops.empty())
    patch(std::move(ops), ops.size() == 1 ? "Move node" : "Move nodes");
  if (!wf_node_.empty())
    wf_reveal_ = wf_node_;
}

void App::delete_in_workflow() {
  const json *workflow = workflow_json();
  if (!workflow)
    return;
  if (!wf_link_.empty()) {
    const std::string link = std::exchange(wf_link_, {});
    patch(json::array({{{"op", "remove"}, {"path", link}}}), "Remove link");
  } else if (!wf_deco_.empty() && wf_node_.empty() && wf_sel_.empty()) { // a frame or a note
    const std::string id = std::exchange(wf_deco_, {});
    const bool is_group = object_in(*workflow, "groups").contains(id);
    patch(json::array({{{"op", "remove"}, {"path", id}}}), is_group ? "Remove frame" : "Remove note");
  } else if (!wf_node_.empty() || !wf_sel_.empty()) {
    std::set<std::string> ids = std::exchange(wf_sel_, {});
    if (!wf_node_.empty())
      ids.insert(std::exchange(wf_node_, {}));
    const json &nodes = object_in(*workflow, "nodes");
    std::erase_if(ids, [&](const std::string &id) { return !nodes.contains(id); });
    if (!ids.empty())
      patch(remove_nodes_ops(*workflow, ids), ids.size() == 1 ? "Remove node" : "Remove nodes");
  }
}

// The ops that keep exactly one Primary Output when the outputs of the open workflow change: `names` are the outputs that
// will exist, `primary` the one marked now.
json App::primary_ops(const std::vector<std::string> &names, const std::string &primary) const {
  json ops = json::array();
  const std::string path = wf_base() + "/exposed/primary";
  if (names.empty()) {
    if (!primary.empty())
      ops.push_back({{"op", "remove"}, {"path", path}});
  } else if (std::find(names.begin(), names.end(), primary) == names.end()) {
    ops.push_back({{"op", primary.empty() ? "add" : "replace"}, {"path", path}, {"value", names.front()}});
  }
  return ops;
}

// The graph: boxes for the nodes, one box for the Exposed Inputs (what the clip sets) and one for the Outputs, curves for
// what joins them. Every port can be dragged, as in ComfyUI: from an output to an input, from an Exposed Input to an
// input (the clip then sets it there), from an output to the Outputs box (the clip then gets it). A port that is already
// joined is picked up with its connection: let go on another port it moves there, let go on nothing it is cut. Cutting
// what an Exposed Input feeds leaves the Exposed Input, unlinked.
void App::draw_workflow_canvas(const json &library) {
  const ImVec2 win = ImGui::GetWindowPos(), size = ImGui::GetWindowSize(), mouse = ImGui::GetIO().MousePos;
  wf_view_ = size;
  ImDrawList *dl = ImGui::GetWindowDrawList();
  // Everything on the canvas is drawn at the zoom: sizes, text and the distances between things.
  const float z = wf_zoom_;
  const float node_w = kNodeW * z, title_h = kNodeTitleH * z, row_h = kPortRowH * z, port_r = std::max(3.0f, kPortR * z), pad = 12.0f * z;
  const float text_px = ImGui::GetFontSize() * z;
  // The background: a click picks the link under the pointer or lets go of the selection, a drag looks around.
  ImGui::SetCursorScreenPos(win);
  ImGui::SetNextItemAllowOverlap();
  ImGui::InvisibleButton("##wf_bg", size);
  ui_mark("workflow_canvas");
  const bool bg_clicked = ImGui::IsItemClicked(), bg_active = ImGui::IsItemActive();
  const ImGuiID bg_id = ImGui::GetItemID();
  const bool bg_hot = ImGui::IsItemHovered();
  const bool bg_dbl = bg_hot && ImGui::IsMouseDoubleClicked(0), bg_right = bg_hot && ImGui::IsMouseClicked(1);
  if (bg_clicked && ImGui::GetIO().KeyShift) { // Shift and a drag on the background: a box that selects what it touches
    wf_box_ = true;
    wf_box_from_ = mouse;
  }
  if (bg_active && !wf_box_ && ImGui::IsMouseDragging(0, 2.0f)) {
    wf_pan_.x += ImGui::GetIO().MouseDelta.x;
    wf_pan_.y += ImGui::GetIO().MouseDelta.y;
    wf_fit_ = false; // the view is the user's now
  }
  if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::GetIO().MouseWheel != 0.0f) { // zoom about the pointer
    const float next = std::clamp(z * std::pow(1.12f, ImGui::GetIO().MouseWheel), 0.35f, 1.6f);
    const ImVec2 at(mouse.x - win.x, mouse.y - win.y);
    wf_pan_ = ImVec2(at.x - (at.x - wf_pan_.x) * next / z, at.y - (at.y - wf_pan_.y) * next / z);
    wf_zoom_ = next;
    wf_fit_ = false;
  }
  const float grid = 28.0f * z;
  for (float x = std::fmod(wf_pan_.x, grid); x < size.x; x += grid) // a dotted grid that moves with the view
    for (float y = std::fmod(wf_pan_.y, grid); y < size.y; y += grid)
      dl->AddRectFilled(ImVec2(win.x + x, win.y + y), ImVec2(win.x + x + 1.5f, win.y + y + 1.5f), hex(look::line, 170));
  const json *open = workflow_json();
  if (!open) {
    const char *hint = "This project has no workflow yet. Add a generative clip from the Generate panel, or make a new workflow on the left.";
    dl->AddText(ImVec2(win.x + (size.x - text_size(hint).x) * 0.5f, win.y + size.y * 0.45f), hex(look::fg3), hint);
    return;
  }
  const json &workflow = *open;
  const json &nodes = object_in(workflow, "nodes"), &links = object_in(workflow, "links");
  const std::vector<gen::ExposedInput> exposed_in = gen::exposed_inputs(library, workflow);
  const std::vector<gen::ExposedOutput> exposed_out = gen::exposed_outputs(workflow);
  const std::string primary = gen::primary_output(workflow);

  // What each node of the open clip made last: asked again after an edit or a Take, and while a run is on.
  if (!wf_clip_.empty() && wf_clip_ == wf_id_ && (wf_results_clip_ != wf_clip_ || wf_results_rev_ != revision_ || (!gen_job_.empty() && clock_ >= next_wf_results_))) {
    next_wf_results_ = clock_ + 1.0;
    wf_results_clip_ = wf_clip_;
    wf_results_rev_ = revision_;
    json got;
    wf_results_ = rpc("gen.node_results", {{"project", project_path_}, {"clip", wf_clip_}}, got) ? got.value("nodes", json::object()) : json::object();
  }
  if (wf_clip_.empty() || wf_clip_ != wf_id_)
    wf_results_ = json::object();
  // The picture of a node: its image output, else a picture or a video among its files.
  const auto preview_of = [&](const std::string &node) -> std::string {
    const auto r = wf_results_.find(node);
    if (r == wf_results_.end() || !r->is_object() || !r->contains("files") || !(*r)["files"].is_object())
      return {};
    const json &files = (*r)["files"];
    std::string best;
    for (auto f = files.begin(); f != files.end(); ++f) {
      if (!f->is_string())
        continue;
      const std::string path = f->get<std::string>();
      const std::string ext = fs::path(path).extension().string();
      if (f.key() == "image" || ext == ".jpg" || ext == ".png")
        return path;
      if (ext == ".mp4" && best.empty())
        best = path;
    }
    return best;
  };
  struct Box {
    std::string id;
    const json *node = nullptr;
    gen::Ports ports;
    ImVec2 pos; // canvas units
    float height = 0.0f;
    std::string preview; // a file whose picture the node shows under its ports
  };
  std::vector<Box> boxes;
  std::map<std::string, size_t> index;
  wf_pos_.clear(); // the positions of this workflow only (the mini-map draws them)
  wf_height_.clear();
  for (auto it = nodes.begin(); it != nodes.end(); ++it) {
    Box b;
    b.id = it.key();
    b.node = &*it;
    b.ports = gen::node_ports(library, *it);
    b.height = node_height(b.ports);
    b.preview = preview_of(b.id);
    if (!b.preview.empty()) {
      b.height += kPreviewH;
      thumbs_.request(b.preview);
    }
    index[b.id] = boxes.size();
    boxes.push_back(std::move(b));
  }
  // What gives each input its value: a link, or the clip (an Exposed Input of the workflow).
  using Key = std::pair<std::string, std::string>;
  std::map<Key, std::string> fed;    // {node, input} -> link id
  std::map<Key, std::string> set_by; // {node, input} -> the Exposed Input that feeds it
  std::string a_node, a_port, b_node, b_port;
  for (auto l = links.begin(); l != links.end(); ++l)
    if (l->contains("from") && l->contains("to") && end_of((*l)["from"], a_node, a_port) && end_of((*l)["to"], b_node, b_port))
      fed[{b_node, b_port}] = l.key();
  for (const gen::ExposedInput &e : exposed_in)
    for (const auto &[node, port] : e.to)
      set_by[{node, port}] = e.name;
  // Where each node is: where it was put (its "ui"), else in a column by how far down the chain it is.
  std::map<std::string, int> depth;
  for (size_t pass = 0; pass < boxes.size(); ++pass)
    for (auto l = links.begin(); l != links.end(); ++l)
      if (l->contains("from") && l->contains("to") && end_of((*l)["from"], a_node, a_port) && end_of((*l)["to"], b_node, b_port))
        depth[b_node] = std::max(depth[b_node], depth[a_node] + 1);
  std::map<int, float> column_y;
  for (Box &b : boxes) {
    const json &ui = object_in(*b.node, "ui");
    if (ui.contains("x") && ui.contains("y") && ui["x"].is_number() && ui["y"].is_number()) {
      b.pos = ImVec2(ui["x"].get<float>(), ui["y"].get<float>());
    } else {
      const int d = depth[b.id];
      b.pos = ImVec2(300.0f + float(d) * (kNodeW + 90.0f), 40.0f + column_y[d]);
      column_y[d] += b.height + 36.0f;
    }
    if (const auto moved = wf_moved_.find(b.id); moved != wf_moved_.end())
      b.pos = moved->second;
    wf_pos_[b.id] = b.pos;
    wf_height_[b.id] = b.height;
  }
  if (!wf_reveal_.empty()) { // a node, frame or note chosen by key: pan the view so that it is on the screen
    ImVec2 where(0.0f, 0.0f);
    float across = kNodeW, down = 0.0f;
    bool found = false;
    if (const auto at = wf_pos_.find(wf_reveal_); at != wf_pos_.end()) {
      where = at->second;
      down = wf_height_[wf_reveal_];
      found = true;
    } else if (const json &gs = object_in(workflow, "groups"); gs.contains(wf_reveal_)) {
      where = ImVec2(gs[wf_reveal_].value("x", 0.0f), gs[wf_reveal_].value("y", 0.0f));
      across = gs[wf_reveal_].value("w", 320.0f);
      down = gs[wf_reveal_].value("h", 200.0f);
      found = true;
    } else if (const json &ns = object_in(workflow, "notes"); ns.contains(wf_reveal_)) {
      where = ImVec2(ns[wf_reveal_].value("x", 0.0f), ns[wf_reveal_].value("y", 0.0f));
      across = 190.0f;
      down = 60.0f;
      found = true;
    }
    if (found) {
      const float margin = 40.0f, left = wf_pan_.x + where.x * z, right = left + across * z;
      const float top = wf_pan_.y + where.y * z, bottom = top + down * z;
      if (left < margin)
        wf_pan_.x += margin - left;
      else if (right > size.x - margin)
        wf_pan_.x -= right - (size.x - margin);
      if (top < margin)
        wf_pan_.y += margin - top;
      else if (bottom > size.y - margin)
        wf_pan_.y -= bottom - (size.y - margin);
      wf_fit_ = false;
    }
    wf_reveal_.clear();
  }
  std::erase_if(wf_sel_, [&](const std::string &id) { return !nodes.contains(id); });
  wf_nodes_total_ = int(nodes.size());
  // What stops a node from running: what is missing (the engine says it for the open clip) and where the last run stopped.
  std::map<std::string, std::string> fail_notes;
  if (!wf_clip_.empty() && wf_clip_ == wf_id_)
    if (const auto found = gen_problems_.find(wf_clip_); found != gen_problems_.end())
      for (const json &problem : found->second) {
        const std::string path = problem.value("path", std::string()), at = path.substr(0, path.find('/'));
        if (nodes.contains(at) && !fail_notes.contains(at)) {
          // The engine words it for an agent, naming the node ("Node nod_... (Title) has no model chosen."); under the node that
          // name says nothing, and it is what a short note would show: it goes.
          std::string message = problem.value("message", std::string());
          if (const std::string head = "Node " + at + " "; message.rfind(head, 0) == 0) {
            message.erase(0, head.size());
            if (const size_t close = message.find(") "); !message.empty() && message[0] == '(' && close != std::string::npos)
              message.erase(0, close + 2);
            if (!message.empty())
              message[0] = char(std::toupper(static_cast<unsigned char>(message[0])));
          }
          fail_notes[at] = message;
        }
      }
  if (!wf_fail_.empty() && wf_fail_clip_ == wf_id_ && wf_fail_hash_ == std::hash<std::string>{}(workflow.dump()))
    for (const auto &[node, why] : wf_fail_)
      if (nodes.contains(node))
        fail_notes[node] = why;
  // The two boxes at the ends: the Exposed Inputs (left of everything) and the Outputs (right of everything).
  float min_x = 300.0f, max_x = 300.0f, min_y = 40.0f;
  for (size_t i = 0; i < boxes.size(); ++i) {
    min_x = i == 0 ? boxes[i].pos.x : std::min(min_x, boxes[i].pos.x);
    max_x = i == 0 ? boxes[i].pos.x : std::max(max_x, boxes[i].pos.x);
    min_y = i == 0 ? boxes[i].pos.y : std::min(min_y, boxes[i].pos.y);
  }
  const ImVec2 in_pos(min_x - 250.0f, min_y), out_pos(max_x + kNodeW + 90.0f, min_y);
  if (wf_fit_) { // the whole graph in view, never larger than life: until the user looks around on their own
    float bottom = min_y + kNodeTitleH + float(std::max(exposed_in.size(), exposed_out.size()) + 1) * kPortRowH;
    for (const Box &b : boxes)
      bottom = std::max(bottom, b.pos.y + b.height);
    const float width = out_pos.x + 190.0f - in_pos.x, height = bottom - min_y;
    wf_zoom_ = std::clamp(std::min((size.x - 80.0f) / std::max(1.0f, width), (size.y - 80.0f) / std::max(1.0f, height)), wf_fit_all_ ? 0.35f : 0.75f, 1.0f);
    wf_pan_ = ImVec2(std::round(std::max(24.0f, (size.x - width * wf_zoom_) * 0.5f) - in_pos.x * wf_zoom_), // centred; from the left edge when it is wider than the view
                     std::round(std::max(40.0f, (size.y - height * wf_zoom_) * 0.4f) - min_y * wf_zoom_));
  }
  const auto screen = [&](ImVec2 p) { return ImVec2(win.x + wf_pan_.x + p.x * z, win.y + wf_pan_.y + p.y * z); };
  const auto in_port = [&](const Box &b, size_t i) { const ImVec2 p = screen(b.pos); return ImVec2(p.x, p.y + title_h + (float(i) + 0.5f) * row_h); };
  const auto out_port = [&](const Box &b, size_t i) { const ImVec2 p = screen(b.pos); return ImVec2(p.x + node_w, p.y + title_h + (float(i) + 0.5f) * row_h); };
  const auto port_index = [](const std::vector<gen::Port> &ports, const std::string &name) {
    for (size_t i = 0; i < ports.size(); ++i)
      if (ports[i].name == name)
        return int(i);
    return -1;
  };
  ImGui::PushFont(g_fonts.ui, text_px); // the text of the canvas, at the zoom; popped at the end of the canvas
  const float hit = std::max(12.0f, 18.0f * z); // a port stays easy to hit when the graph is small

  // Every port of the graph. A source gives a value (a node's output, a row of the Exposed Inputs); a sink takes one (a
  // node's input, a row of the Outputs). The last row of each side box is "new": a name that does not exist yet.
  struct End {
    bool source = false;
    int where = 0; // 0: a port of a node; 1: a named row of a side box; 2: the "new" row of a side box
    std::string node, port, name;
    gen::Port def;      // the port (of a node), or the type of the row
    bool typed = false; // def is known
    ImVec2 at;
  };
  std::vector<End> ends;
  for (const Box &b : boxes) {
    for (size_t i = 0; i < b.ports.inputs.size(); ++i)
      ends.push_back({false, 0, b.id, b.ports.inputs[i].name, {}, b.ports.inputs[i], true, in_port(b, i)});
    for (size_t i = 0; i < b.ports.outputs.size(); ++i)
      ends.push_back({true, 0, b.id, b.ports.outputs[i].name, {}, b.ports.outputs[i], true, out_port(b, i)});
  }
  const float side_w = 190.0f * z, side_head = title_h - pad;
  const ImVec2 in_at = screen(in_pos), out_at = screen(out_pos);
  const auto side_row = [&](ImVec2 box, bool inputs, size_t row) { return ImVec2(inputs ? box.x + side_w : box.x, box.y + side_head + (float(row) + 0.5f) * row_h); };
  for (size_t i = 0; i <= exposed_in.size(); ++i) {
    const bool fresh = i == exposed_in.size();
    gen::Port def;
    if (!fresh)
      def = gen::Port{exposed_in[i].name, exposed_in[i].type, exposed_in[i].required, exposed_in[i].list};
    ends.push_back({true, fresh ? 2 : 1, {}, {}, fresh ? std::string() : exposed_in[i].name, def, !fresh, side_row(in_at, true, i)});
  }
  for (size_t i = 0; i <= exposed_out.size(); ++i) {
    const bool fresh = i == exposed_out.size();
    gen::Port def;
    bool typed = false;
    if (!fresh && index.count(exposed_out[i].node))
      if (const gen::Port *p = boxes[index[exposed_out[i].node]].ports.output(exposed_out[i].port)) {
        def = *p;
        def.name = exposed_out[i].name;
        typed = true;
      }
    ends.push_back({false, fresh ? 2 : 1, fresh ? std::string() : exposed_out[i].node, fresh ? std::string() : exposed_out[i].port,
                    fresh ? std::string() : exposed_out[i].name, def, typed, side_row(out_at, false, i)});
  }
  // What a clip can set or get: values and media. Conditioning and latents live in the engine and travel by links only.
  const auto for_clip = [](gen::PortType t) { return t != gen::PortType::conditioning && t != gen::PortType::latent; };
  // May `source` give its value to `sink`?
  const auto joins = [&](const End &source, const End &sink) {
    if (!source.source || sink.source || (source.where != 0 && sink.where != 0))
      return false; // not a source and a sink, or the two sides of the clip to each other
    if (source.where == 0 && sink.where == 0)
      return source.node != sink.node && gen::can_link(source.def, sink.def);
    const End &port = source.where == 0 ? source : sink, &row = source.where == 0 ? sink : source;
    if (!for_clip(port.def.type))
      return false;
    if (row.where == 2 || !row.typed)
      return true;
    return source.where == 0 ? gen::can_link(source.def, row.def) : gen::can_link(row.def, sink.def); // an Exposed Input keeps the type it says
  };

  // The connection that is being dragged, if any: the end that is held, and what was picked up with it.
  const End *held = nullptr;
  if (wf_drag_.active)
    for (const End &e : ends)
      if (e.source == wf_drag_.source && e.where == wf_drag_.where && e.node == wf_drag_.node && e.port == wf_drag_.port && e.name == wf_drag_.name)
        held = &e;
  if (wf_drag_.active && !held)
    wf_drag_ = {};
  const End *target = nullptr; // the port the loose end would join
  if (held) {
    float best = std::max(11.0f, 14.0f * z);
    for (const End &e : ends) {
      const float d = std::hypot(mouse.x - e.at.x, mouse.y - e.at.y);
      if (d < best && (held->source ? joins(*held, e) : joins(e, *held))) {
        best = d;
        target = &e;
      }
    }
    if (!target) { // anywhere on a side box is its "new" row
      const bool on_in = !held->source && mouse.x >= in_at.x && mouse.x <= in_at.x + side_w && mouse.y >= in_at.y &&
                         mouse.y <= in_at.y + side_head + float(exposed_in.size() + 1) * row_h + 10.0f * z;
      const bool on_out = held->source && mouse.x >= out_at.x && mouse.x <= out_at.x + side_w && mouse.y >= out_at.y &&
                          mouse.y <= out_at.y + side_head + float(exposed_out.size() + 1) * row_h + 10.0f * z;
      for (const End &e : ends)
        if (e.where == 2 && ((on_in && e.source) || (on_out && !e.source)) && (held->source ? joins(*held, e) : joins(e, *held)))
          target = &e;
    }
  }
  const auto fits = [&](const End &e) { return held && (held->source ? joins(*held, e) : joins(e, *held)); };
  const auto is_target = [&](const End &e) { return target == &e; };
  const auto end_at = [&](bool source, int where, const std::string &node, const std::string &port, const std::string &name) -> const End * {
    for (const End &e : ends)
      if (e.source == source && e.where == where && e.node == node && e.port == port && e.name == name)
        return &e;
    return nullptr;
  };
  // A press on a port starts a drag from it.
  const auto dot_button = [&](const End &e, const char *id, const std::string &mark) {
    ImGui::SetCursorScreenPos(ImVec2(e.at.x - hit * 0.5f, e.at.y - hit * 0.5f));
    ImGui::InvisibleButton(id, ImVec2(hit, hit));
    ui_mark(mark);
    return ImGui::IsItemActivated();
  };
  const auto start = [&](bool source, int where, const std::string &node, const std::string &port, const std::string &name) {
    wf_drag_ = {};
    wf_drag_.active = true;
    wf_drag_.source = source;
    wf_drag_.where = where;
    wf_drag_.node = node;
    wf_drag_.port = port;
    wf_drag_.name = name;
    wf_node_.clear();
    wf_link_.clear();
    wf_row_.clear();
  };

  // Frames and notes: under everything. A frame is moved by its title bar, with the nodes inside it, and resized by its corner; a note
  // is moved by its body. A click selects: the side panel edits the title or the text.
  {
    const auto live = [&](const std::string &id, const json &v, bool group) {
      const auto it = wf_deco_live_.find(id);
      if (it != wf_deco_live_.end())
        return it->second;
      return std::array<float, 4>{v.value("x", 0.0f), v.value("y", 0.0f), group ? v.value("w", 320.0f) : 190.0f, group ? v.value("h", 200.0f) : 0.0f};
    };
    const auto colour_of = [](const json &v, int alpha) {
      unsigned rgb = 0x4a90d9;
      const std::string c = v.value("color", std::string());
      if (c.size() == 7 && c[0] == '#')
        rgb = unsigned(std::strtoul(c.c_str() + 1, nullptr, 16));
      return IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, alpha);
    };
    const json &groups = object_in(workflow, "groups"), &notes = object_in(workflow, "notes");
    for (auto g = groups.begin(); g != groups.end(); ++g) {
      const std::string gid = g.key();
      const std::array<float, 4> r = live(gid, *g, true);
      const ImVec2 p = screen(ImVec2(r[0], r[1])), q(p.x + r[2] * z, p.y + r[3] * z);
      const bool sel = wf_deco_ == gid;
      dl->AddRectFilled(p, q, colour_of(*g, 30), 10.0f * z);
      dl->AddRect(p, q, colour_of(*g, sel ? 255 : 150), 10.0f * z, 0, sel ? 2.4f : 1.4f);
      const std::string title = g->value("title", std::string("Group"));
      ImGui::PushFont(g_fonts.bold, 13.0f * z);
      dl->AddText(ImVec2(p.x + 12.0f * z, p.y + 7.0f * z), colour_of(*g, 255), title.c_str());
      ImGui::PopFont();
      // the title bar: a press selects, a drag moves the frame and what is inside it
      ImGui::SetCursorScreenPos(p);
      ImGui::SetNextItemAllowOverlap();
      ImGui::InvisibleButton(("##grp_" + gid).c_str(), ImVec2(r[2] * z, 30.0f * z));
      {
        std::string joined = title;
        std::replace(joined.begin(), joined.end(), ' ', '_');
        ui_mark("group:" + joined);
      }
      if (ImGui::IsItemActivated()) {
        wf_deco_ = gid;
        wf_node_.clear();
        wf_sel_.clear();
        wf_row_.clear();
        wf_link_.clear();
        wf_deco_drag_ = {};
        wf_deco_drag_.id = gid;
        wf_deco_drag_.mode = 1;
        wf_deco_drag_.from = r;
        for (const Box &b : boxes) { // the nodes whose middle is inside
          const ImVec2 mid(b.pos.x + kNodeW * 0.5f, b.pos.y + b.height * 0.5f);
          if (mid.x >= r[0] && mid.x <= r[0] + r[2] && mid.y >= r[1] && mid.y <= r[1] + r[3]) {
            wf_deco_drag_.nodes.push_back(b.id);
            wf_deco_drag_.nodes_at[b.id] = b.pos;
          }
        }
      }
      if (ImGui::IsItemActive() && wf_deco_drag_.id == gid && wf_deco_drag_.mode == 1 && ImGui::IsMouseDragging(0, 3.0f)) {
        const ImVec2 d = ImGui::GetMouseDragDelta(0);
        wf_deco_live_[gid] = {wf_deco_drag_.from[0] + d.x / z, wf_deco_drag_.from[1] + d.y / z, r[2], r[3]};
        for (const auto &[nid, at] : wf_deco_drag_.nodes_at)
          wf_moved_[nid] = ImVec2(at.x + d.x / z, at.y + d.y / z);
        wf_fit_ = false;
      }
      // the corner: a drag changes its size
      const ImVec2 corner(q.x - 16.0f * z, q.y - 16.0f * z);
      ImGui::SetCursorScreenPos(corner);
      ImGui::SetNextItemAllowOverlap();
      ImGui::InvisibleButton(("##grpsize_" + gid).c_str(), ImVec2(16.0f * z, 16.0f * z));
      dl->AddTriangleFilled(ImVec2(q.x - 3.0f, q.y - 14.0f * z), ImVec2(q.x - 3.0f, q.y - 3.0f), ImVec2(q.x - 14.0f * z, q.y - 3.0f), colour_of(*g, 190));
      if (ImGui::IsItemActivated()) {
        wf_deco_ = gid;
        wf_deco_drag_ = {};
        wf_deco_drag_.id = gid;
        wf_deco_drag_.mode = 2;
        wf_deco_drag_.from = r;
      }
      if (ImGui::IsItemActive() && wf_deco_drag_.id == gid && wf_deco_drag_.mode == 2 && ImGui::IsMouseDragging(0, 2.0f)) {
        const ImVec2 d = ImGui::GetMouseDragDelta(0);
        wf_deco_live_[gid] = {r[0], r[1], std::max(80.0f, wf_deco_drag_.from[2] + d.x / z), std::max(60.0f, wf_deco_drag_.from[3] + d.y / z)};
      }
    }
    for (auto n = notes.begin(); n != notes.end(); ++n) {
      const std::string nid = n.key();
      const std::array<float, 4> r = live(nid, *n, false);
      const std::string text = n->value("text", std::string());
      // wrapped to the width, so its height follows the text
      std::vector<std::string> lines;
      {
        std::string line, word;
        const float room = (r[2] - 20.0f) * z;
        const auto flush = [&] {
          if (!line.empty())
            lines.push_back(line);
          line.clear();
        };
        for (size_t i = 0; i <= text.size(); ++i) {
          const char ch = i < text.size() ? text[i] : ' ';
          if (ch == ' ' || ch == '\n') {
            const std::string trial = line.empty() ? word : line + " " + word;
            if (!line.empty() && text_size(trial.c_str()).x > room) {
              flush();
              line = word;
            } else {
              line = trial;
            }
            word.clear();
            if (ch == '\n')
              flush();
          } else {
            word += ch;
          }
        }
        flush();
        if (lines.empty())
          lines.push_back("");
      }
      const float line_h = ImGui::GetFontSize() + 3.0f * z, height = float(lines.size()) * line_h + 18.0f * z;
      const ImVec2 p = screen(ImVec2(r[0], r[1])), q(p.x + r[2] * z, p.y + height);
      const bool sel = wf_deco_ == nid;
      dl->AddRectFilled(p, q, IM_COL32(240, 205, 90, 235), 6.0f * z);
      if (sel)
        dl->AddRect(p, q, hex(look::accent), 6.0f * z, 0, 2.4f);
      for (size_t i = 0; i < lines.size(); ++i)
        dl->AddText(ImVec2(p.x + 10.0f * z, p.y + 9.0f * z + float(i) * line_h), IM_COL32(50, 40, 10, 255), lines[i].c_str());
      ImGui::SetCursorScreenPos(p);
      ImGui::SetNextItemAllowOverlap();
      ImGui::InvisibleButton(("##note_" + nid).c_str(), ImVec2(r[2] * z, height));
      ui_mark("note_card:" + text.substr(0, 24));
      if (ImGui::IsItemActivated()) {
        wf_deco_ = nid;
        wf_node_.clear();
        wf_sel_.clear();
        wf_row_.clear();
        wf_link_.clear();
        wf_deco_drag_ = {};
        wf_deco_drag_.id = nid;
        wf_deco_drag_.mode = 1;
        wf_deco_drag_.from = r;
      }
      if (ImGui::IsItemActive() && wf_deco_drag_.id == nid && ImGui::IsMouseDragging(0, 3.0f)) {
        const ImVec2 d = ImGui::GetMouseDragDelta(0);
        wf_deco_live_[nid] = {wf_deco_drag_.from[0] + d.x / z, wf_deco_drag_.from[1] + d.y / z, r[2], 0.0f};
        wf_fit_ = false;
      }
    }
    // A move or a resize that was let go: one edit for the frame or note and the nodes that went with it.
    if (!ImGui::IsMouseDown(0) && !wf_deco_live_.empty() && !wf_deco_drag_.id.empty()) {
      json ops = json::array();
      const std::string id = wf_deco_drag_.id;
      if (const auto it = wf_deco_live_.find(id); it != wf_deco_live_.end()) {
        const std::array<float, 4> r = it->second;
        const bool is_group = groups.contains(id);
        const json &was = is_group ? groups[id] : notes.contains(id) ? notes[id] : object_in(json::object(), "");
        const auto set = [&](const char *field, double value) {
          ops.push_back({{"op", was.contains(field) ? "replace" : "add"}, {"path", id + "/" + field}, {"value", value}});
        };
        set("x", std::round(r[0]));
        set("y", std::round(r[1]));
        if (is_group) {
          set("w", std::round(r[2]));
          set("h", std::round(r[3]));
        }
        for (const auto &[nid, at] : wf_moved_)
          if (nodes.contains(nid))
            ops.push_back({{"op", nodes[nid].contains("ui") ? "replace" : "add"}, {"path", nid + "/ui"}, {"value", {{"x", std::round(at.x)}, {"y", std::round(at.y)}}}});
      }
      wf_deco_drag_ = {};
      pending_ = [this, ops] {
        if (!ops.empty())
          patch(ops, ops.size() > 4 ? "Move frame" : "Move");
        wf_deco_live_.clear();
        wf_moved_.clear();
      };
    }
  }

  // Links, under the boxes. The one under the pointer is found here for the click on the background.
  std::string link_hit;
  for (auto l = links.begin(); l != links.end(); ++l) {
    if (!l->contains("from") || !l->contains("to") || !end_of((*l)["from"], a_node, a_port) || !end_of((*l)["to"], b_node, b_port) ||
        !index.count(a_node) || !index.count(b_node) || l.key() == wf_drag_.cut_link)
      continue; // a link that was picked up is drawn at the pointer instead
    const Box &from = boxes[index[a_node]], &to = boxes[index[b_node]];
    const int fi = port_index(from.ports.outputs, a_port), ti = port_index(to.ports.inputs, b_port);
    if (fi < 0 || ti < 0)
      continue;
    const ImVec2 p1 = out_port(from, size_t(fi)), p2 = in_port(to, size_t(ti));
    const bool selected = l.key() == wf_link_;
    draw_link(dl, p1, p2, selected ? hex(look::accent) : port_colour(from.ports.outputs[size_t(fi)].type, 220), (selected ? 3.5f : 2.4f) * z);
    if (curve_distance(mouse, p1, p2) < 7.0f)
      link_hit = l.key();
  }
  if (bg_clicked) {
    wf_link_ = link_hit;
    if (!ImGui::GetIO().KeyShift) { // a click on nothing lets go of the selection; with Shift it starts a box
      wf_node_.clear();
      wf_sel_.clear();
      wf_deco_.clear();
    }
    wf_row_.clear();
    wf_search_.open = false;
  }

  // The Exposed Inputs box: one row each, in their order. A row that feeds nothing is dimmed: it keeps the clip's value.
  {
    const ImVec2 p = in_at;
    const float h = side_head + float(exposed_in.size() + 1) * row_h + 10.0f * z;
    dl->AddRectFilled(p, ImVec2(p.x + side_w, p.y + h), hex(look::panel), 10.0f * z);
    dl->AddRect(p, ImVec2(p.x + side_w, p.y + h), hex(look::line2), 10.0f * z, 0, 1.2f);
    ImGui::PushFont(g_fonts.bold, 12.0f * z);
    dl->AddText(ImVec2(p.x + pad, p.y + 9.0f * z), hex(look::fg2), "CLIP INPUTS");
    ImGui::PopFont();
    for (size_t row = 0; row <= exposed_in.size(); ++row) {
      const bool fresh = row == exposed_in.size();
      const End *e = end_at(true, fresh ? 2 : 1, {}, {}, fresh ? std::string() : exposed_in[row].name);
      if (!e)
        continue;
      const ImVec2 dot = e->at;
      const gen::ExposedInput *input = fresh ? nullptr : &exposed_in[row];
      const bool unlinked = input && input->to.empty();
      const std::string label = fresh ? "new input" : input->label.empty() ? input->name : input->label;
      const ImU32 colour = fresh ? hex(look::fg3) : port_colour(input->type, unlinked ? 110 : 255);
      if (input)
        for (const auto &[node, port] : input->to) {
          if (node == wf_drag_.cut_node && port == wf_drag_.cut_port && input->name == wf_drag_.cut_in)
            continue; // picked up: drawn at the pointer
          if (!index.count(node))
            continue;
          const Box &b = boxes[index[node]];
          if (const int i = port_index(b.ports.inputs, port); i >= 0)
            draw_link(dl, dot, in_port(b, size_t(i)), port_colour(input->type, 170), 2.0f * z);
        }
      if (fits(*e))
        dl->AddCircle(dot, port_r + (is_target(*e) ? 6.0f : 3.5f) * z, held && held->typed ? port_colour(held->def.type, is_target(*e) ? 255 : 130) : hex(look::fg2), 0, 2.0f);
      if (fresh) { // a hollow port: drag from it, or to it
        dl->AddCircleFilled(dot, port_r, hex(look::panel));
        dl->AddCircle(dot, port_r, colour, 0, 1.6f);
      } else if (unlinked) {
        dl->AddCircleFilled(dot, port_r, hex(look::panel));
        dl->AddCircle(dot, port_r, colour, 0, 1.8f);
      } else {
        dl->AddCircleFilled(dot, port_r, colour);
      }
      const ImVec2 ts = text_size(label.c_str());
      if (!fresh) { // the row: a click on its name selects it, and the side panel renames, moves or removes it
        const bool on = wf_row_ == "in:" + input->name;
        ImGui::SetCursorScreenPos(ImVec2(in_at.x + 4.0f * z, dot.y - row_h * 0.5f));
        ImGui::InvisibleButton(("##cinrow_" + input->name).c_str(), ImVec2(side_w - 4.0f * z - port_r * 3.0f, row_h));
        ui_mark("row:in:" + input->name);
        if (ImGui::IsItemClicked()) {
          wf_row_ = "in:" + input->name;
          wf_node_.clear();
          wf_link_.clear();
        }
        if (on)
          dl->AddRectFilled(ImVec2(in_at.x + 4.0f * z, dot.y - row_h * 0.5f), ImVec2(in_at.x + side_w - port_r * 3.0f, dot.y + row_h * 0.5f), hex(look::accent, 40), 5.0f * z);
      }
      dl->AddText(ImVec2(dot.x - pad - ts.x, dot.y - ts.y * 0.5f), hex(fresh || unlinked ? look::fg3 : look::fg), label.c_str());
      if (dot_button(*e, ("##cin_" + (fresh ? std::string("+") : input->name)).c_str(), std::string("clipin:") + (fresh ? "+" : input->name)))
        start(true, e->where, {}, {}, e->name);
      if (ImGui::IsItemHovered() && !held) {
        ImGui::PushFont(g_fonts.ui, text_px / z);
        if (fresh)
          ImGui::SetTooltip("Drag to an input of a node: the clip will set it.");
        else
          ImGui::SetTooltip("%s  (%s)%s\nDrag to an input of the same type to feed it.", input->name.c_str(), gen::port_type_name(input->type),
                            unlinked ? "\nIt feeds nothing: the clip's value is kept, and does nothing." : "");
        ImGui::PopFont();
      }
    }
  }
  // The Outputs box: one row each, the Primary Output marked.
  {
    const ImVec2 p = out_at;
    const float h = side_head + float(exposed_out.size() + 1) * row_h + 10.0f * z;
    dl->AddRectFilled(p, ImVec2(p.x + side_w, p.y + h), hex(look::panel), 10.0f * z);
    dl->AddRect(p, ImVec2(p.x + side_w, p.y + h), hex(look::line2), 10.0f * z, 0, 1.2f);
    ImGui::PushFont(g_fonts.bold, 12.0f * z);
    dl->AddText(ImVec2(p.x + pad, p.y + 9.0f * z), hex(look::fg2), "OUTPUT");
    ImGui::PopFont();
    for (size_t row = 0; row <= exposed_out.size(); ++row) {
      const bool fresh = row == exposed_out.size();
      const End *e = end_at(false, fresh ? 2 : 1, fresh ? std::string() : exposed_out[row].node, fresh ? std::string() : exposed_out[row].port,
                            fresh ? std::string() : exposed_out[row].name);
      if (!e)
        continue;
      const ImVec2 dot = e->at;
      const std::string label = fresh ? "new output" : exposed_out[row].name;
      const bool cut = !fresh && exposed_out[row].name == wf_drag_.cut_out;
      const ImU32 colour = fresh || !e->typed ? hex(look::fg3) : port_colour(e->def.type);
      if (!fresh && e->typed && !cut)
        if (const auto &o = exposed_out[row]; index.count(o.node))
          if (const int i = port_index(boxes[index[o.node]].ports.outputs, o.port); i >= 0)
            draw_link(dl, out_port(boxes[index[o.node]], size_t(i)), dot, port_colour(e->def.type, 170), 2.0f * z);
      if (fits(*e))
        dl->AddCircle(dot, port_r + (is_target(*e) ? 6.0f : 3.5f) * z, held && held->typed ? port_colour(held->def.type, is_target(*e) ? 255 : 130) : hex(look::fg2), 0, 2.0f);
      if (fresh) {
        dl->AddCircleFilled(dot, port_r, hex(look::panel));
        dl->AddCircle(dot, port_r, colour, 0, 1.6f);
      } else {
        dl->AddCircleFilled(dot, port_r, colour);
      }
      if (!fresh) {
        const bool on = wf_row_ == "out:" + exposed_out[row].name;
        ImGui::SetCursorScreenPos(ImVec2(out_at.x + port_r * 3.0f, dot.y - row_h * 0.5f));
        ImGui::InvisibleButton(("##coutrow_" + exposed_out[row].name).c_str(), ImVec2(side_w - port_r * 3.0f - 4.0f * z, row_h));
        ui_mark("row:out:" + exposed_out[row].name);
        if (ImGui::IsItemClicked()) {
          wf_row_ = "out:" + exposed_out[row].name;
          wf_node_.clear();
          wf_link_.clear();
        }
        if (on)
          dl->AddRectFilled(ImVec2(out_at.x + port_r * 3.0f, dot.y - row_h * 0.5f), ImVec2(out_at.x + side_w - 4.0f * z, dot.y + row_h * 0.5f), hex(look::accent, 40), 5.0f * z);
      }
      dl->AddText(ImVec2(dot.x + pad, dot.y - text_size(label.c_str()).y * 0.5f), hex(fresh ? look::fg3 : look::fg), label.c_str());
      if (!fresh && exposed_out[row].name == primary) { // the Primary Output: a small mark on the right
        const char *mark = "main";
        dl->AddText(ImVec2(dot.x + side_w - pad - text_size(mark).x, dot.y - text_size(mark).y * 0.5f), hex(look::accent), mark);
      }
      if (dot_button(*e, ("##cout_" + (fresh ? std::string("+") : exposed_out[row].name)).c_str(), std::string("clipout:") + (fresh ? "+" : exposed_out[row].name))) {
        if (fresh) { // a sink that waits for an output
          start(false, 2, {}, {}, {});
        } else if (e->typed) { // picked up: the output it shows is held, its name is let go
          start(true, 0, exposed_out[row].node, exposed_out[row].port, {});
          wf_drag_.cut_out = exposed_out[row].name;
        }
      }
      if (ImGui::IsItemHovered() && !held) {
        ImGui::PushFont(g_fonts.ui, text_px / z);
        ImGui::SetTooltip(fresh ? "Drag an output of a node here: the clip will get it." : "The clip gets \"%s\"%s.\nDrag it away to take it back.", label.c_str(),
                          !fresh && exposed_out[row].name == primary ? ", and plays it" : "");
        ImGui::PopFont();
      }
    }
  }

  // The nodes.
  for (Box &b : boxes) {
    const ImVec2 p = screen(b.pos), q(p.x + node_w, p.y + b.height * z);
    const std::string kind = b.node->value("kind", std::string());
    const gen::KindDef *def = gen::find_kind(kind);
    const std::string model = b.node->value("model", std::string());
    const bool selected = b.id == wf_node_ || wf_sel_.contains(b.id);
    const bool kind_is_workflow = gen::is_workflow_kind(kind);
    // The body: select and move. A click selects it; Shift or Ctrl adds it to the selection, or takes it out; a node of several that
    // are selected moves them all.
    ImGui::SetCursorScreenPos(p);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton(("##node_" + b.id).c_str(), ImVec2(node_w, b.height * z));
    ui_mark("node:" + kind_id(kind));
    ui_mark("node:" + b.id);
    if (ImGui::IsItemActivated()) {
      const ImGuiIO &keys = ImGui::GetIO();
      if (keys.KeyShift || keys.KeyCtrl) {
        if (!wf_sel_.erase(b.id))
          wf_sel_.insert(b.id);
      } else if (!wf_sel_.contains(b.id)) {
        wf_sel_ = {b.id};
      }
      wf_node_ = wf_sel_.size() == 1 ? *wf_sel_.begin() : std::string();
      wf_link_.clear();
      wf_row_.clear();
      wf_deco_.clear();
    }
    if (kind_is_workflow && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) { // a Subgraph: double click opens the workflow inside
      const std::string inner = b.node->value("workflow", std::string());
      if (library.contains(inner))
        pending_ = [this, inner] { open_workflow(inner); };
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 3.0f)) {
      const ImVec2 d = ImGui::GetMouseDragDelta(0, 3.0f);
      ImGui::ResetMouseDragDelta(0);
      for (const std::string &id : wf_sel_) {
        const auto moved = wf_moved_.find(id);
        const ImVec2 now = moved != wf_moved_.end() ? moved->second : wf_pos_[id];
        wf_moved_[id] = ImVec2(now.x + d.x / z, now.y + d.y / z);
      }
      wf_fit_ = false; // the view holds still while nodes are moved in it
    }
    if (ImGui::IsItemDeactivated()) {
      if (!wf_moved_.empty()) { // the move is one edit, for every node that was moved
        json ops = json::array();
        for (const auto &[id, at] : wf_moved_) {
          const auto node = nodes.find(id);
          if (node == nodes.end())
            continue;
          ops.push_back({{"op", node->contains("ui") ? "replace" : "add"}, {"path", id + "/ui"},
                         {"value", {{"x", std::round(at.x)}, {"y", std::round(at.y)}}}});
        }
        pending_ = [this, ops] {
          if (!ops.empty())
            patch(ops, ops.size() == 1 ? "Move node" : "Move nodes");
          wf_moved_.clear();
        };
      } else if (wf_sel_.size() > 1 && !ImGui::GetIO().KeyShift && !ImGui::GetIO().KeyCtrl && ImGui::IsItemHovered()) { // a plain click on one of several
        wf_sel_ = {b.id};
        wf_node_ = b.id;
      }
    }
    const bool hovered = ImGui::IsItemHovered();
    dl->AddRectFilled(p, q, hex(look::panel2), 10.0f * z);
    dl->AddRectFilled(p, ImVec2(q.x, p.y + title_h), hex(def && def->is_input ? look::accent2 : look::gen, selected ? 120 : 70), 10.0f * z, ImDrawFlags_RoundCornersTop);
    ImGui::PushFont(g_fonts.bold, 13.0f * z);
    dl->AddText(ImVec2(p.x + pad, p.y + 7.0f * z), hex(look::fg), kind_title(kind).c_str());
    ImGui::PopFont();
    std::string sub = model;
    bool sub_bad = false;
    if (gen::is_workflow_kind(kind)) {
      const std::string inner = b.node->value("workflow", std::string());
      sub = library.contains(inner) ? library[inner].value("name", inner) : "no workflow";
    } else if (def && def->runs_model && model.empty()) {
      sub = "No model chosen";
      sub_bad = true;
    } else if (def && std::string_view(def->id) == "project") {
      sub = "Canvas size and frame rate";
    } else if (def && std::string_view(def->id) == "clip") {
      sub = "Its length and start";
    } else if (def && std::string_view(def->id) == "variable") {
      const std::string var = b.node->value("variable", std::string());
      const json &vars = object_in(doc_, "variables");
      sub = var.empty() ? "No variable chosen" : vars.contains(var) ? "\"" + vars[var].value("name", var) + "\"" : "Not in the project";
      sub_bad = var.empty() || !vars.contains(var);
    } else if (def && std::string_view(def->id) == "clip_reference") {
      const std::string named = object_in(*b.node, "settings").value("clip", std::string());
      const ClipUi *other = named.empty() || named == "previous" || named == "next" ? nullptr : find_clip(named);
      sub = named.empty() ? "No clip chosen" : named == "previous" ? "The clip before" : named == "next" ? "The clip after" : other ? other->name : "Not a clip";
      sub_bad = named.empty() || (named != "previous" && named != "next" && !other);
    } else if (def && std::string_view(def->id) == "get_frame") {
      sub = object_in(*b.node, "settings").value("frame", std::string("last")) == "first" ? "The first frame" : "The last frame";
    } else if (!def) {
      sub = "Not a node kind of this version";
      sub_bad = true;
    }
    dl->PushClipRect(p, ImVec2(q.x - 8.0f * z, q.y), true);
    dl->AddText(ImVec2(p.x + pad, p.y + 25.0f * z), sub_bad ? ImGui::ColorConvertFloat4ToU32(kError) : hex(look::fg2), sub.c_str());
    dl->PopClipRect();
    const json &typed = object_in(*b.node, "inputs");
    bool missing = sub_bad;
    for (size_t i = 0; i < b.ports.inputs.size(); ++i) {
      const gen::Port &port = b.ports.inputs[i];
      const End *e = end_at(false, 0, b.id, port.name, {});
      if (!e)
        continue;
      const ImVec2 c = e->at;
      const auto link = fed.find({b.id, port.name});
      const auto name = set_by.find({b.id, port.name});
      // A connection that was picked up is not here any more, as far as the picture goes.
      const bool has_link = link != fed.end() && link->second != wf_drag_.cut_link;
      const bool exposed = name != set_by.end() && !(b.id == wf_drag_.cut_node && port.name == wf_drag_.cut_port);
      const bool has_value = typed.contains(port.name);
      const bool needs = port.required && link == fed.end() && name == set_by.end() && !has_value;
      missing = missing || needs;
      if (fits(*e))
        dl->AddCircle(c, port_r + (is_target(*e) ? 6.0f : 3.5f) * z, port_colour(port.type, is_target(*e) ? 255 : 130), 0, 2.0f);
      // An optional input that nothing is joined to stays in the background: dim, no words about it.
      const bool quiet = !port.required && !has_link && !exposed && !has_value;
      if (has_link || exposed) {
        dl->AddCircleFilled(c, port_r, port_colour(port.type));
      } else {
        dl->AddCircleFilled(c, port_r, hex(look::panel2));
        dl->AddCircle(c, port_r, needs ? ImGui::ColorConvertFloat4ToU32(kError) : port_colour(port.type, quiet ? 110 : 255), 0, 1.8f);
      }
      std::string label = port.name;
      if (has_value && !has_link && !exposed) { // a value kept in the node: shown next to the name
        const json &v = typed[port.name];
        const std::string text = v.is_string() ? v.get<std::string>() : v.dump();
        label += "  " + (text.size() > 14 ? text.substr(0, 13) + ".." : text);
      }
      dl->AddText(ImVec2(c.x + pad, c.y - ImGui::GetFontSize() * 0.5f),
                  needs ? ImGui::ColorConvertFloat4ToU32(kError) : hex(quiet ? look::fg3 : port.required ? look::fg : look::fg2), label.c_str());
      if (dot_button(*e, ("##in_" + b.id + "_" + port.name).c_str(), "port:" + kind_id(kind) + "." + port.name)) {
        if (link != fed.end()) { // picked up: the output that fed it is held
          std::string from_node, from_port;
          if (end_of(links[link->second].value("from", json::array()), from_node, from_port)) {
            start(true, 0, from_node, from_port, {});
            wf_drag_.cut_link = link->second;
          }
        } else if (name != set_by.end()) { // picked up: the Exposed Input that feeds it is held, and this feed let go
          start(true, 1, {}, {}, name->second);
          wf_drag_.cut_in = name->second;
          wf_drag_.cut_node = b.id;
          wf_drag_.cut_port = port.name;
        } else { // nothing joined: the input is held and looks for an output
          start(false, 0, b.id, port.name, {});
        }
      }
      if (ImGui::IsItemHovered() && !held) {
        ImGui::PushFont(g_fonts.ui, text_px / z);
        ImGui::SetTooltip("%s  (%s%s)%s", port.name.c_str(), gen::port_type_name(port.type), port.required ? "" : ", optional",
                          needs ? "\nNothing gives it a value yet: drag an output or an Exposed Input onto it, or type a value on the right."
                          : has_link || exposed ? "\nDrag it away to break the connection." : "");
        ImGui::PopFont();
      }
    }
    for (size_t i = 0; i < b.ports.outputs.size(); ++i) {
      const gen::Port &port = b.ports.outputs[i];
      const End *e = end_at(true, 0, b.id, port.name, {});
      if (!e)
        continue;
      const ImVec2 c = e->at;
      if (fits(*e))
        dl->AddCircle(c, port_r + (is_target(*e) ? 6.0f : 3.5f) * z, port_colour(port.type, is_target(*e) ? 255 : 130), 0, 2.0f);
      dl->AddCircleFilled(c, port_r, port_colour(port.type));
      const ImVec2 ts = text_size(port.name.c_str());
      dl->AddText(ImVec2(c.x - pad - ts.x, c.y - ts.y * 0.5f), hex(look::fg2), port.name.c_str());
      if (dot_button(*e, ("##out_" + b.id + "_" + port.name).c_str(), "port:" + kind_id(kind) + "." + port.name + ":out"))
        start(true, 0, b.id, port.name, {});
      if (ImGui::IsItemHovered() && !held) {
        ImGui::PushFont(g_fonts.ui, text_px / z);
        ImGui::SetTooltip("%s  (%s)\nDrag to an input of the same colour, or to what the clip gets.", port.name.c_str(), gen::port_type_name(port.type));
        ImGui::PopFont();
      }
    }
    if (!b.preview.empty()) { // what the node made last: the picture under its ports
      const ImVec2 a(p.x + 8.0f * z, q.y - (kPreviewH - 6.0f) * z), c(q.x - 8.0f * z, q.y - 8.0f * z);
      dl->AddRectFilled(a, c, hex(look::bg), 6.0f * z);
      if (const auto tex = thumb_tex_.find(b.preview); tex != thumb_tex_.end() && tex->second) {
        float tw = c.x - a.x, th = c.y - a.y;
        if (SDL_GetTextureSize(tex->second, &tw, &th) && tw > 0.0f && th > 0.0f) {
          const float fit = std::min((c.x - a.x) / tw, (c.y - a.y) / th);
          tw *= fit;
          th *= fit;
        }
        const ImVec2 at((a.x + c.x - tw) * 0.5f, (a.y + c.y - th) * 0.5f);
        dl->AddImageRounded(ImTextureID(reinterpret_cast<intptr_t>(tex->second)), at, ImVec2(at.x + tw, at.y + th), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 5.0f * z);
      }
      ui_mark("preview:" + b.id);
    }
    if (gen_job_state_.is_object() && gen_job_state_.contains("node") && gen_job_state_["node"].is_object() && !gen_job_.empty() &&
        gen_job_state_["node"].value("id", std::string()) == b.id) { // the node that is running: a bar along its foot
      const int at = gen_job_state_["node"].value("at", 0), of = gen_job_state_["node"].value("of", 0);
      const float fraction = of > 0 ? float(at) / float(of) : float(std::fmod(clock_ * 0.8, 1.0));
      const ImVec2 a(p.x + 8.0f * z, q.y - 7.0f * z), c(q.x - 8.0f * z, q.y - 3.0f * z);
      dl->AddRectFilled(a, c, hex(look::raised), 2.0f * z);
      dl->AddRectFilled(a, ImVec2(a.x + (c.x - a.x) * std::clamp(fraction, 0.02f, 1.0f), c.y), hex(look::accent), 2.0f * z);
      if (of > 0) {
        char step[24];
        std::snprintf(step, sizeof step, "%d of %d", at, of);
        dl->AddText(ImVec2(q.x - pad - text_size(step).x, p.y + 7.0f * z), hex(look::accent), step);
      }
      ui_mark("running:" + b.id);
      missing = false;
    }
    const auto note = fail_notes.find(b.id);
    if (note != fail_notes.end())
      missing = true;
    dl->AddRect(p, q, selected ? hex(look::accent) : missing ? ImGui::ColorConvertFloat4ToU32(kError) : hex(hovered ? look::line2 : look::line), 10.0f * z, 0,
                selected ? 2.0f : 1.3f);
    if (note != fail_notes.end()) { // why: up to three lines of red under the node, and the whole of it when the pointer is on the node
      const float room = node_w - 2.0f * pad;
      std::vector<std::string> lines(1);
      size_t at = 0;
      for (size_t next = 0; at < note->second.size() && lines.size() <= 3; at = next) { // word by word
        next = note->second.find(' ', at + 1);
        const std::string word = note->second.substr(at, next == std::string::npos ? std::string::npos : next - at);
        if (next == std::string::npos)
          next = note->second.size();
        if (!lines.back().empty() && text_size((lines.back() + word).c_str()).x > room)
          lines.push_back(word.substr(word[0] == ' ' ? 1 : 0));
        else
          lines.back() += lines.back().empty() && word[0] == ' ' ? word.substr(1) : word;
      }
      if (lines.size() > 3) { // more than fits: the third line ends in dots
        lines.resize(3);
        while (lines[2].size() > 4 && text_size((lines[2] + "...").c_str()).x > room)
          lines[2].pop_back();
        lines[2] += "...";
      }
      for (size_t i = 0; i < lines.size(); ++i)
        dl->AddText(ImVec2(p.x + pad, q.y + 4.0f * z + float(i) * ImGui::GetFontSize()), ImGui::ColorConvertFloat4ToU32(kError), lines[i].c_str());
      ui_mark("note:" + b.id);
      if (hovered) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(340.0f);
        ImGui::TextColored(kError, "%s", note->second.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
      }
    }
  }

  // The connection being dragged: a curve from the held port to the pointer, and on release the edit.
  if (held) {
    const ImU32 colour = held->typed ? port_colour(held->def.type, 230) : hex(look::fg2);
    const ImVec2 loose = target ? target->at : mouse;
    draw_link(dl, held->source ? held->at : loose, held->source ? loose : held->at, colour, 2.4f * z);
    if (!ImGui::IsMouseDown(0)) {
      const WfDrag drag = wf_drag_;
      const std::string base = wf_base();
      json ops = json::array();
      std::string label = "Link";
      const auto input_named = [&](const std::string &name) -> const gen::ExposedInput * {
        for (const gen::ExposedInput &i : exposed_in)
          if (i.name == name)
            return &i;
        return nullptr;
      };
      // The list of ports an Exposed Input feeds, as a patch value.
      const auto to_list = [](const std::vector<std::pair<std::string, std::string>> &ends) {
        json list = json::array();
        for (const auto &[node, port] : ends)
          list.push_back(json::array({node, port}));
        return list;
      };
      // What was picked up goes: a link, a feed of an Exposed Input (the Exposed Input stays), or an Output (the name goes).
      std::map<std::string, std::vector<std::pair<std::string, std::string>>> feeds; // Exposed Input -> what it feeds after the edit
      for (const gen::ExposedInput &i : exposed_in)
        feeds[i.name] = i.to;
      std::set<std::string> feeds_changed;
      std::vector<std::string> outputs_after;
      for (const gen::ExposedOutput &o : exposed_out)
        outputs_after.push_back(o.name);
      bool outputs_changed = false;
      const auto cut = [&] {
        if (!drag.cut_link.empty())
          ops.push_back({{"op", "remove"}, {"path", drag.cut_link}});
        if (!drag.cut_in.empty()) {
          auto &to = feeds[drag.cut_in];
          to.erase(std::remove(to.begin(), to.end(), std::make_pair(drag.cut_node, drag.cut_port)), to.end());
          feeds_changed.insert(drag.cut_in);
        }
        if (!drag.cut_out.empty()) {
          ops.push_back({{"op", "remove"}, {"path", base + "/exposed/outputs/" + drag.cut_out}});
          outputs_after.erase(std::remove(outputs_after.begin(), outputs_after.end(), drag.cut_out), outputs_after.end());
          outputs_changed = true;
        }
      };
      const auto fresh_name = [](const auto &used, const std::string &wanted) {
        std::string name = wanted;
        for (int n = 2; std::any_of(used.begin(), used.end(), [&](const auto &u) { return u.name == name; }); ++n)
          name = wanted + "_" + std::to_string(n);
        return name;
      };
      int64_t next_order = 0;
      for (const gen::ExposedInput &i : exposed_in)
        next_order = std::max(next_order, i.order + 1);
      if (!target) { // let go on nothing: the connection is broken; a link that was not joined to anything asks for a node
        cut();
        label = drag.cut_out.empty() ? "Disconnect" : "Take back an output";
        if (ops.empty() && drag.where == 0 && held->typed && !drag.node.empty()) {
          wf_search_ = {};
          wf_search_.open = wf_search_.focus = wf_search_.linked = true;
          wf_search_.screen = mouse;
          wf_search_.canvas = ImVec2((mouse.x - win.x - wf_pan_.x) / z, (mouse.y - win.y - wf_pan_.y) / z);
          wf_search_.from_output = held->source;
          wf_search_.node = drag.node;
          wf_search_.port = drag.port;
          wf_search_.type = int(held->def.type);
        }
      } else {
        const End &source = held->source ? *held : *target, &sink = held->source ? *target : *held;
        if (sink.where == 0) { // onto an input of a node
          const auto link = fed.find({sink.node, sink.port});
          const auto name = set_by.find({sink.node, sink.port});
          const bool back = (link != fed.end() && link->second == drag.cut_link) ||
                            (name != set_by.end() && name->second == drag.cut_in && sink.node == drag.cut_node && sink.port == drag.cut_port);
          if (!back) { // put back where it came from changes nothing
            cut();
            // What gave the input its value until now gives way: a link, a value typed in the node, another Exposed Input.
            if (link != fed.end() && link->second != drag.cut_link)
              ops.push_back({{"op", "remove"}, {"path", link->second}});
            if (object_in(nodes[sink.node], "inputs").contains(sink.port))
              ops.push_back({{"op", "remove"}, {"path", sink.node + "/inputs/" + sink.port}});
            if (name != set_by.end()) {
              auto &to = feeds[name->second];
              to.erase(std::remove(to.begin(), to.end(), std::make_pair(sink.node, sink.port)), to.end());
              feeds_changed.insert(name->second);
            }
            if (source.where == 0) { // an output of a node: a link
              ops.push_back({{"op", "add"}, {"path", base + "/links/$new:l"}, {"value", {{"from", {source.node, source.port}}, {"to", {sink.node, sink.port}}}}});
            } else if (source.where == 1) { // an Exposed Input: it feeds this input as well
              feeds[source.name].push_back({sink.node, sink.port});
              feeds_changed.insert(source.name);
              label = "Let the clip set an input";
            } else { // a new Exposed Input, with the type of the input it feeds
              const gen::Port &port = sink.def;
              const std::string fresh = fresh_name(exposed_in, sink.port);
              ops.push_back({{"op", "add"},
                             {"path", base + "/exposed/inputs/" + fresh},
                             {"value", {{"type", gen::port_type_name(port.type)}, {"order", next_order}, {"to", to_list({{sink.node, sink.port}})}}}});
              label = "Let the clip set an input";
            }
          }
        } else if (sink.where == 1) { // onto an Output that exists: it shows this output now
          if (drag.cut_out != sink.name) {
            cut();
            ops.push_back({{"op", "replace"}, {"path", base + "/exposed/outputs/" + sink.name + "/from"}, {"value", json::array({source.node, source.port})}});
            label = "Change what the clip gets";
          }
        } else if (drag.cut_out.empty()) { // a new Output
          const std::string fresh = fresh_name(exposed_out, source.port);
          ops.push_back({{"op", "add"}, {"path", base + "/exposed/outputs/" + fresh}, {"value", {{"from", {source.node, source.port}}}}});
          outputs_after.push_back(fresh);
          outputs_changed = true;
          label = "Give the clip an output";
        }
      }
      for (const std::string &name : feeds_changed)
        if (input_named(name))
          ops.push_back({{"op", "replace"}, {"path", base + "/exposed/inputs/" + name + "/to"}, {"value", to_list(feeds[name])}});
      if (outputs_changed)
        for (json &op : primary_ops(outputs_after, primary))
          ops.push_back(std::move(op));
      if (!ops.empty())
        pending_ = [this, ops, label] {
          json ids;
          if (patch(ops, label.c_str(), &ids))
            wf_link_ = ids.value("$new:l", "");
        };
      wf_drag_ = {};
    }
  }
  ImGui::PopFont();
  // The box of Shift and a drag: what it touches is selected when the button is let go.
  if (wf_box_) {
    const ImVec2 lo(std::min(wf_box_from_.x, mouse.x), std::min(wf_box_from_.y, mouse.y)), hi(std::max(wf_box_from_.x, mouse.x), std::max(wf_box_from_.y, mouse.y));
    dl->AddRectFilled(lo, hi, hex(look::accent, 28));
    dl->AddRect(lo, hi, hex(look::accent, 200), 0.0f, 0, 1.2f);
    if (!ImGui::IsMouseDown(0)) {
      wf_box_ = false;
      for (const Box &b : boxes) {
        const ImVec2 p = screen(b.pos), q(p.x + node_w, p.y + b.height * z);
        if (p.x < hi.x && q.x > lo.x && p.y < hi.y && q.y > lo.y)
          wf_sel_.insert(b.id);
      }
      wf_node_ = wf_sel_.size() == 1 ? *wf_sel_.begin() : std::string();
      wf_link_.clear();
      wf_row_.clear();
    }
  }
  // L and I with the keys on the canvas: the chosen node's outputs or inputs as a list; Enter holds one, or (a port is held) links it to the held one.
  if (const char request = std::exchange(wf_ports_request_, '\0'); request != 0) {
    wf_ports_ = {};
    const auto found = nodes.find(wf_node_);
    if (wf_node_.empty() || found == nodes.end()) {
      say("Choose a node first (the arrows or Tab).");
    } else if (wf_hold_.active && wf_hold_.source == (request == 'L')) {
      say(wf_hold_.source ? "An output is held: choose the other node and press I for its inputs." : "An input is held: choose the other node and press L for its outputs.");
    } else {
      const gen::Ports mine = gen::node_ports(library, *found);
      const bool want_outputs = request == 'L';
      wf_ports_.outputs = want_outputs;
      wf_ports_.node = wf_node_;
      for (const gen::Port &port : want_outputs ? mine.outputs : mine.inputs) {
        if (wf_hold_.active && wf_hold_.node == wf_node_)
          continue; // not to itself
        if (wf_hold_.active) { // only what fits the held end
          const gen::Port holding{wf_hold_.port, gen::PortType(wf_hold_.type), false, false};
          if (!(want_outputs ? gen::can_link(port, holding) : gen::can_link(holding, port)))
            continue;
        }
        wf_ports_.rows.emplace_back(port.name, int(port.type));
      }
      if (wf_ports_.rows.empty())
        say(wf_hold_.active ? "No port of this node fits the held one." : "This node has no such ports.");
      else
        wf_ports_.open = true;
    }
  }
  if (wf_ports_.open) {
    const auto at = wf_pos_.find(wf_ports_.node);
    if (at == wf_pos_.end() || wf_ports_.rows.empty()) {
      wf_ports_.open = false;
    } else {
      if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true))
        wf_ports_.pick = std::min(wf_ports_.pick + 1, int(wf_ports_.rows.size()) - 1);
      if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true))
        wf_ports_.pick = std::max(wf_ports_.pick - 1, 0);
      const float w = 250.0f, row = 28.0f, head = 34.0f, h = head + float(wf_ports_.rows.size()) * row + 8.0f;
      ImVec2 box(win.x + wf_pan_.x + (at->second.x + kNodeW + 16.0f) * z, win.y + wf_pan_.y + at->second.y * z);
      box.x = std::clamp(box.x, win.x + 8.0f, win.x + size.x - w - 8.0f);
      box.y = std::clamp(box.y, win.y + 8.0f, win.y + size.y - h - 8.0f);
      ImGui::SetCursorScreenPos(box);
      ImGui::SetNextItemAllowOverlap();
      ImGui::InvisibleButton("##wf_ports_block", ImVec2(w, h)); // nothing under the box gets its clicks
      dl->AddRectFilled(box, ImVec2(box.x + w, box.y + h), hex(look::panel), 10.0f);
      dl->AddRect(box, ImVec2(box.x + w, box.y + h), hex(look::line2), 10.0f, 0, 1.3f);
      const char *title = wf_hold_.active ? (wf_ports_.outputs ? "Link from one of these" : "Link to one of these") : (wf_ports_.outputs ? "Outputs: hold one" : "Inputs: hold one");
      dl->AddText(ImVec2(box.x + 12.0f, box.y + 8.0f), hex(look::fg3), title);
      int chosen = -1;
      for (size_t i = 0; i < wf_ports_.rows.size(); ++i) {
        const ImVec2 rp(box.x + 6.0f, box.y + head + float(i) * row);
        ImGui::SetCursorScreenPos(rp);
        ImGui::InvisibleButton(("##wf_port_" + std::to_string(i)).c_str(), ImVec2(w - 12.0f, row - 2.0f));
        ui_mark("wf_port:" + wf_ports_.rows[i].first);
        if (ImGui::IsItemHovered() || int(i) == wf_ports_.pick)
          dl->AddRectFilled(rp, ImVec2(rp.x + w - 12.0f, rp.y + row - 2.0f), hex(look::raised), 6.0f);
        dl->AddText(ImVec2(rp.x + 10.0f, rp.y + 5.0f), hex(look::fg), wf_ports_.rows[i].first.c_str());
        const char *type = gen::port_type_name(gen::PortType(wf_ports_.rows[i].second));
        dl->AddText(ImVec2(rp.x + w - 24.0f - text_size(type).x, rp.y + 5.0f), hex(look::fg3), type);
        if (ImGui::IsItemClicked())
          chosen = int(i);
      }
      if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
        chosen = wf_ports_.pick;
      if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        wf_ports_.open = false;
      if (chosen >= 0 && chosen < int(wf_ports_.rows.size())) {
        const std::string port = wf_ports_.rows[size_t(chosen)].first;
        const int type = wf_ports_.rows[size_t(chosen)].second;
        const std::string node = wf_ports_.node;
        const bool outputs = wf_ports_.outputs;
        wf_ports_.open = false;
        if (!wf_hold_.active) { // hold it
          wf_hold_ = {true, outputs, node, port, type};
          say(std::string("Holding ") + port + ": choose the other node, then press " + (outputs ? "I" : "L") + ". A adds a new node that fits. Esc lets go.");
        } else { // link it to the held one
          const std::string from_node = outputs ? node : wf_hold_.node, from_port = outputs ? port : wf_hold_.port;
          const std::string to_node = outputs ? wf_hold_.node : node, to_port = outputs ? wf_hold_.port : port;
          wf_hold_ = {};
          if (set_by.count({to_node, to_port})) {
            say("That input is fed by the clip: disconnect it in the side panel first.", true);
          } else {
            json ops = json::array();
            if (const auto old = fed.find({to_node, to_port}); old != fed.end()) // what fed the input gives way
              ops.push_back({{"op", "remove"}, {"path", old->second}});
            if (object_in(nodes[to_node], "inputs").contains(to_port)) // so does a value typed into it
              ops.push_back({{"op", "remove"}, {"path", to_node + "/inputs/" + to_port}});
            ops.push_back({{"op", "add"}, {"path", wf_base() + "/links/$new:l"}, {"value", {{"from", {from_node, from_port}}, {"to", {to_node, to_port}}}}});
            pending_ = [this, ops] { patch(ops, "Link"); };
          }
        }
      }
    }
  }
  // A or / with the keys on the canvas: the same search, at the right of the chosen node (else in the middle of the view).
  if (wf_search_key_) {
    wf_search_key_ = false;
    wf_search_ = {};
    wf_search_.open = wf_search_.focus = true;
    if (wf_hold_.active) { // a port is held: only the nodes that can take it (or give to it) are listed, and the new one is linked
      wf_search_.linked = true;
      wf_search_.from_output = wf_hold_.source;
      wf_search_.node = wf_hold_.node;
      wf_search_.port = wf_hold_.port;
      wf_search_.type = wf_hold_.type;
      wf_hold_ = {};
    }
    const auto at = wf_pos_.find(wf_node_);
    wf_search_.canvas = at != wf_pos_.end() ? ImVec2(at->second.x + kNodeW + 50.0f, at->second.y)
                                            : ImVec2((size.x * 0.5f - wf_pan_.x) / z, (size.y * 0.35f - wf_pan_.y) / z);
    wf_search_.screen = ImVec2(win.x + wf_pan_.x + wf_search_.canvas.x * z, win.y + wf_pan_.y + wf_search_.canvas.y * z);
    wf_link_.clear();
  }
  // Double click or right click on the background: the Node Library as a search, where the pointer is.
  if ((bg_dbl || bg_right) && ImGui::GetCurrentContext()->HoveredId == bg_id && !wf_drag_.active) {
    wf_search_ = {};
    wf_search_.open = wf_search_.focus = true;
    wf_search_.screen = mouse;
    wf_search_.canvas = ImVec2((mouse.x - win.x - wf_pan_.x) / z, (mouse.y - win.y - wf_pan_.y) / z);
    wf_link_.clear();
  }
  if (wf_search_.open) {
    const json kinds = wf_parts_.value("kinds", json::array());
    std::string needle = wf_search_.text;
    std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    std::vector<std::pair<std::string, std::string>> shown; // id, title
    for (const json &k : kinds) {
      const std::string kid = k.value("id", ""), title = k.value("title", kid);
      std::string hay = title + " " + kid;
      std::transform(hay.begin(), hay.end(), hay.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
      if (!needle.empty() && hay.find(needle) == std::string::npos)
        continue;
      if (wf_search_.linked) { // only what can take the held output, or give to the held input
        const gen::Port loose{wf_search_.port, gen::PortType(wf_search_.type), false, false};
        bool fits_it = false;
        for (const json &port : k.value(wf_search_.from_output ? "inputs" : "outputs", json::array())) {
          gen::PortType type = gen::PortType::text;
          if (!gen::port_type_from_name(port.value("type", ""), type))
            continue;
          const gen::Port other{port.value("name", ""), type, false, port.value("list", false)};
          fits_it = fits_it || (wf_search_.from_output ? gen::can_link(loose, other) : gen::can_link(other, loose));
        }
        if (!fits_it)
          continue;
      }
      shown.emplace_back(kid, title);
    }
    if (needle != wf_search_.last) { // the words changed: the first row again
      wf_search_.last = needle;
      wf_search_.pick = 0;
    }
    if (!shown.empty()) {
      if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true))
        wf_search_.pick = std::min(wf_search_.pick + 1, int(shown.size()) - 1);
      if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true))
        wf_search_.pick = std::max(wf_search_.pick - 1, 0);
      wf_search_.pick = std::clamp(wf_search_.pick, 0, int(shown.size()) - 1);
    }
    const float w = 250.0f, row = 28.0f, head = 46.0f;
    const float h = head + std::max(1.0f, float(shown.size())) * row + 10.0f;
    ImVec2 at = wf_search_.screen;
    at.x = std::clamp(at.x, win.x + 8.0f, win.x + size.x - w - 8.0f);
    at.y = std::clamp(at.y, win.y + 8.0f, win.y + size.y - h - 8.0f);
    ImGui::SetCursorScreenPos(at);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##wf_search_block", ImVec2(w, h)); // nothing under the box gets its clicks
    dl->AddRectFilled(at, ImVec2(at.x + w, at.y + h), hex(look::panel), 10.0f, 0);
    dl->AddRect(at, ImVec2(at.x + w, at.y + h), hex(look::line2), 10.0f, 0, 1.3f);
    ImGui::SetCursorScreenPos(ImVec2(at.x + 10.0f, at.y + 10.0f));
    if (wf_search_.focus) {
      ImGui::SetKeyboardFocusHere();
      wf_search_.focus = false;
    }
    ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
    ImGui::SetNextItemWidth(w - 20.0f);
    const bool entered = ImGui::InputTextWithHint("##wf_search_text", wf_search_.linked ? "Add a node that fits" : "Add a node", wf_search_.text, sizeof wf_search_.text,
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleColor();
    ui_mark("field:wf_search");
    std::string chosen;
    for (size_t i = 0; i < shown.size(); ++i) {
      const ImVec2 rp(at.x + 6.0f, at.y + head + float(i) * row);
      ImGui::SetCursorScreenPos(rp);
      ImGui::InvisibleButton(("##wf_search_" + shown[i].first).c_str(), ImVec2(w - 12.0f, row - 2.0f));
      ui_mark("wf_search:" + shown[i].first);
      if (ImGui::IsItemHovered() || int(i) == wf_search_.pick)
        dl->AddRectFilled(rp, ImVec2(rp.x + w - 12.0f, rp.y + row - 2.0f), hex(look::raised), 6.0f);
      dl->AddText(ImVec2(rp.x + 10.0f, rp.y + 5.0f), hex(look::fg), shown[i].second.c_str());
      if (ImGui::IsItemClicked())
        chosen = shown[i].first;
    }
    if (shown.empty())
      dl->AddText(ImVec2(at.x + 16.0f, at.y + head + 5.0f), hex(look::fg3), "No node fits.");
    if (entered && !shown.empty())
      chosen = shown[size_t(std::clamp(wf_search_.pick, 0, int(shown.size()) - 1))].first;
    if (!chosen.empty())
      pending_ = [this, chosen] { wf_add_from_search(chosen); };
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
      wf_search_.open = false;
  }
  if (boxes.empty()) {
    const char *hint = "An empty workflow. Add a node from the list on the left, or double click here.";
    dl->AddText(ImVec2(win.x + (size.x - text_size(hint).x) * 0.5f, win.y + size.y * 0.45f), hex(look::fg3), hint);
  }
  // Over the graph, top left, for the clip whose workflow this is: make it from here, and see how far it is.
  if (!wf_clip_.empty() && wf_clip_ == wf_id_) {
    const auto known = gen_state_.find(wf_clip_);
    const std::string state = known != gen_state_.end() ? known->second.value("state", "empty") : std::string("empty");
    const bool needs = state == "dirty" || state == "empty";
    ImGui::SetCursorScreenPos(ImVec2(win.x + 12.0f, win.y + 8.0f));
    if (!gen_job_.empty()) {
      if (soft_button("wf_gen_stop", "Stop", ImVec2(80.0f, 28.0f))) {
        json unused;
        rpc("jobs.cancel", {{"job_id", gen_job_}}, unused);
      }
      ImGui::SameLine(0.0f, 10.0f);
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.0f);
      ImGui::PushStyleColor(ImGuiCol_PlotHistogram, hexv(look::accent));
      ImGui::ProgressBar(float(gen_job_state_.value("progress", 0.0)), ImVec2(140.0f, 6.0f), "");
      ImGui::PopStyleColor();
      ImGui::SameLine(0.0f, 10.0f);
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 6.0f);
      ImGui::TextColored(hexv(look::fg3), "%s", gen_job_state_.value("detail", "Starting").c_str());
    } else {
      const std::string clip = wf_clip_;
      if (soft_button("wf_generate", needs ? "Generate" : "New take", ImVec2(110.0f, 28.0f), state != "locked", needs))
        pending_ = [this, clip, needs] { start_generation({{"clips", json::array({clip})}, {"new_take", !needs}}); };
      if (state == "clean") {
        ImGui::SameLine(0.0f, 10.0f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(hexv(look::ok), "Up to date");
      } else if (gen_job_state_.value("state", "") == "failed") {
        ImGui::SameLine(0.0f, 10.0f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kError, "The run stopped%s", wf_fail_.empty() ? "." : ": see the node.");
      }
    }
  }
  // Over the graph, top right: how large it is drawn, and back to the whole of it.
  char scale[16];
  std::snprintf(scale, sizeof scale, "%d%%", int(std::lround(wf_zoom_ * 100.0f)));
  ImGui::SetCursorScreenPos(ImVec2(win.x + size.x - 124.0f, win.y + 12.0f));
  ImGui::PushFont(g_fonts.mono, 12.0f);
  ImGui::TextColored(hexv(look::fg3), "%5s", scale);
  ImGui::PopFont();
  ImGui::SetCursorScreenPos(ImVec2(win.x + size.x - 68.0f, win.y + 8.0f));
  if (soft_button("wf_fit", "Fit", ImVec2(56.0f, 26.0f), true, false, wf_fit_ && wf_fit_all_ ? look::line2 : look::raised)) {
    wf_fit_ = true;
    wf_fit_all_ = true;
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Show the whole workflow. The mouse wheel zooms, a drag on the background looks around.");
  // The way in for a new node: a button, as well as a double click on the background.
  ImGui::SetCursorScreenPos(ImVec2(win.x + size.x - 188.0f, win.y + 8.0f));
  if (soft_button("wf_add_node", "+ Add node", ImVec2(108.0f, 26.0f), true, true)) {
    wf_search_ = WfSearch{};
    wf_search_.open = true;
    wf_search_.focus = true;
    wf_search_.screen = ImVec2(win.x + size.x * 0.5f, win.y + size.y * 0.35f);
    wf_search_.canvas = ImVec2((wf_search_.screen.x - win.x - wf_pan_.x) / std::max(0.01f, wf_zoom_), (wf_search_.screen.y - win.y - wf_pan_.y) / std::max(0.01f, wf_zoom_));
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Add a node: a search opens. A double click on the background does the same, at the pointer.");
  // A mini-map, bottom right, when the graph is larger than the view: every node as a small box, the view as a frame; a click or a drag moves the view.
  if (!wf_pos_.empty()) {
    float lo_x = 1e9f, lo_y = 1e9f, hi_x = -1e9f, hi_y = -1e9f;
    for (const auto &[id, pos] : wf_pos_) {
      lo_x = std::min(lo_x, pos.x);
      lo_y = std::min(lo_y, pos.y);
      hi_x = std::max(hi_x, pos.x + kNodeW);
      hi_y = std::max(hi_y, pos.y + 160.0f);
    }
    const ImVec2 view_lo((0.0f - wf_pan_.x) / std::max(0.01f, wf_zoom_), (0.0f - wf_pan_.y) / std::max(0.01f, wf_zoom_));
    const ImVec2 view_hi((size.x - wf_pan_.x) / std::max(0.01f, wf_zoom_), (size.y - wf_pan_.y) / std::max(0.01f, wf_zoom_));
    const bool larger = view_lo.x > lo_x + 4.0f || view_lo.y > lo_y + 4.0f || view_hi.x < hi_x - 4.0f || view_hi.y < hi_y - 4.0f;
    if (larger || wf_nodes_total_ > 8) {
      lo_x = std::min(lo_x, view_lo.x);
      lo_y = std::min(lo_y, view_lo.y);
      hi_x = std::max(hi_x, view_hi.x);
      hi_y = std::max(hi_y, view_hi.y);
      const float mw = 170.0f, mh = 110.0f;
      const float k = std::min(mw / std::max(1.0f, hi_x - lo_x), mh / std::max(1.0f, hi_y - lo_y));
      const ImVec2 box_lo(win.x + size.x - mw - 16.0f, win.y + size.y - mh - 16.0f);
      ImDrawList *mdl = ImGui::GetWindowDrawList();
      mdl->AddRectFilled(ImVec2(box_lo.x - 6.0f, box_lo.y - 6.0f), ImVec2(box_lo.x + mw + 6.0f, box_lo.y + mh + 6.0f), hex(look::panel, 235), 8.0f);
      mdl->AddRect(ImVec2(box_lo.x - 6.0f, box_lo.y - 6.0f), ImVec2(box_lo.x + mw + 6.0f, box_lo.y + mh + 6.0f), hex(look::line2), 8.0f);
      const auto to_map = [&](ImVec2 p) { return ImVec2(box_lo.x + (p.x - lo_x) * k, box_lo.y + (p.y - lo_y) * k); };
      for (const auto &[id, pos] : wf_pos_)
        mdl->AddRectFilled(to_map(pos), to_map(ImVec2(pos.x + kNodeW, pos.y + 100.0f)), wf_sel_.count(id) ? hex(look::accent) : hex(look::fg3), 2.0f);
      mdl->AddRect(to_map(view_lo), to_map(view_hi), hex(look::accent), 2.0f, 0, 1.5f);
      ImGui::SetCursorScreenPos(box_lo);
      ImGui::InvisibleButton("##minimap", ImVec2(mw, mh));
      ui_mark("workflow_minimap");
      if (ImGui::IsItemActive()) { // the view's middle goes to the pointer
        const ImVec2 at = ImGui::GetIO().MousePos;
        const ImVec2 centre_on(lo_x + (at.x - box_lo.x) / k, lo_y + (at.y - box_lo.y) / k);
        wf_pan_ = ImVec2(size.x * 0.5f - centre_on.x * wf_zoom_, size.y * 0.5f - centre_on.y * wf_zoom_);
        wf_fit_ = false;
      }
    }
  }
}

// The right column: the selected node (its model, settings and inputs), else the workflow itself.
void App::draw_workflow_side(const json &library) {
  const json *open = workflow_json();
  if (!open) {
    ImGui::TextColored(hexv(look::fg3), "No workflow is open.");
    return;
  }
  const json &workflow = *open;
  const json &nodes = object_in(workflow, "nodes"), &links = object_in(workflow, "links");
  const std::vector<gen::ExposedInput> exposed_in = gen::exposed_inputs(library, workflow);
  const std::vector<gen::ExposedOutput> exposed_out = gen::exposed_outputs(workflow);
  const std::string primary = gen::primary_output(workflow);
  const std::string base = wf_base();
  const bool of_clip = id_prefix(wf_id_) == "clp";
  const ClipUi *clip = of_clip ? find_clip(wf_id_) : nullptr;
  const float label_w = 104.0f;

  // A text field over a value of the document: the buffer follows the document until it is typed in, and the edit is
  // made when the field is left.
  const auto text_field = [&](const std::string &key, const std::string &current, float width, const std::function<void(const std::string &)> &commit) {
    std::array<char, 512> &buf = wf_text_[key];
    if (wf_editing_ != key)
      copy_to(buf.data(), buf.size(), current);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
    ImGui::SetNextItemWidth(width);
    ImGui::InputText(("##" + key).c_str(), buf.data(), buf.size());
    ImGui::PopStyleColor();
    if (ImGui::IsItemActive())
      wf_editing_ = key;
    else if (wf_editing_ == key)
      wf_editing_.clear();
    if (ImGui::IsItemDeactivatedAfterEdit() && current != buf.data())
      commit(buf.data());
  };

  // A frame or a note of the canvas: its title or text, its colour, remove.
  if (!wf_deco_.empty()) {
    const json &groups = object_in(workflow, "groups"), &notes = object_in(workflow, "notes");
    const bool is_group = groups.contains(wf_deco_), is_note = notes.contains(wf_deco_);
    if (!is_group && !is_note) {
      wf_deco_.clear();
    } else {
      const json &v = is_group ? groups[wf_deco_] : notes[wf_deco_];
      const std::string did = wf_deco_;
      if (begin_card("##wf_deco", is_group ? "Frame" : "Note")) {
        if (is_group) {
          ImGui::TextColored(hexv(look::fg2), "Title");
          ImGui::SameLine(label_w);
          text_field("grp:" + did, v.value("title", std::string()), -1.0f, [this, did, had = v.contains("title")](const std::string &typed) {
            pending_ = [this, did, typed, had] { patch(json::array({{{"op", had ? "replace" : "add"}, {"path", did + "/title"}, {"value", typed}}}), "Rename frame"); };
          });
          ui_mark("field:wf_group_title");
          ImGui::TextColored(hexv(look::fg2), "Colour");
          static const char *colours[] = {"#4a90d9", "#3fa66b", "#d98a3a", "#c0504d", "#9a6ad0"};
          for (const char *colour : colours) {
            ImGui::SameLine(colour == colours[0] ? label_w : 0.0f);
            unsigned rgb = unsigned(std::strtoul(colour + 1, nullptr, 16));
            const ImVec2 cp = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton((std::string("##col") + colour).c_str(), ImVec2(24.0f, 24.0f)))
              pending_ = [this, did, colour = std::string(colour), had = v.contains("color")] {
                patch(json::array({{{"op", had ? "replace" : "add"}, {"path", did + "/color"}, {"value", colour}}}), "Colour frame");
              };
            ui_mark(std::string("button:wf_group_colour_") + (colour + 1));
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(cp.x + 2.0f, cp.y + 2.0f), ImVec2(cp.x + 22.0f, cp.y + 22.0f), IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, 255), 5.0f);
            if (v.value("color", std::string()) == colour)
              ImGui::GetWindowDrawList()->AddRect(ImVec2(cp.x, cp.y), ImVec2(cp.x + 24.0f, cp.y + 24.0f), hex(look::fg), 6.0f, 0, 1.5f);
          }
          ImGui::PushTextWrapPos(0.0f);
          ImGui::TextColored(hexv(look::fg3), "Drag its title bar to move it with the nodes inside, its corner to change its size. A frame changes no result.");
          ImGui::PopTextWrapPos();
        } else {
          std::array<char, 512> &buf = wf_text_["note:" + did];
          if (wf_editing_ != "note:" + did)
            copy_to(buf.data(), buf.size(), v.value("text", std::string()));
          ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
          ImGui::InputTextMultiline("##note_text", buf.data(), buf.size(), ImVec2(-1.0f, 110.0f));
          ImGui::PopStyleColor();
          ui_mark("field:wf_note_text");
          if (ImGui::IsItemActive())
            wf_editing_ = "note:" + did;
          else if (wf_editing_ == "note:" + did)
            wf_editing_.clear();
          if (ImGui::IsItemDeactivatedAfterEdit() && v.value("text", std::string()) != buf.data())
            pending_ = [this, did, text = std::string(buf.data()), had = v.contains("text")] {
              patch(json::array({{{"op", had ? "replace" : "add"}, {"path", did + "/text"}, {"value", text}}}), "Edit note");
            };
        }
      }
      end_card();
      if (soft_button("wf_deco_remove", is_group ? "Remove this frame" : "Remove this note", ImVec2(-1.0f, 30.0f)))
        pending_ = [this, did, is_group] {
          if (patch(json::array({{{"op", "remove"}, {"path", did}}}), is_group ? "Remove frame" : "Remove note"))
            wf_deco_.clear();
        };
      return;
    }
  }
  // A row of the Clip Inputs node or of the Output node: its name, its place, and what can be done with it.
  if (!wf_row_.empty()) {
    const bool is_in = wf_row_.rfind("in:", 0) == 0;
    const std::string name = wf_row_.substr(is_in ? 3 : 4);
    const gen::ExposedInput *input = nullptr;
    const gen::ExposedOutput *output = nullptr;
    size_t at = 0;
    for (size_t i = 0; i < exposed_in.size(); ++i)
      if (is_in && exposed_in[i].name == name) {
        input = &exposed_in[i];
        at = i;
      }
    for (const gen::ExposedOutput &o : exposed_out)
      if (!is_in && o.name == name)
        output = &o;
    if (!input && !output) {
      wf_row_.clear();
    } else {
      static const json no_media = json::object();
      const json *self_doc = of_clip ? clip_json(wf_id_) : nullptr;
      const json &media = self_doc ? object_in(*self_doc, "media_ref") : no_media;
      const json &have = object_in(media, "inputs");
      if (begin_card("##wf_row", is_in ? "Input of the clip" : "Output of the clip")) {
        const std::string label = is_in ? input->label : std::string();
        ImGui::TextColored(hexv(look::fg2), "Name");
        ImGui::SameLine(label_w);
        text_field("row:" + wf_row_, label.empty() ? name : label, -1.0f, [this, is_in, name, label, &have, &exposed_in, &exposed_out, base, of_clip, input, output](const std::string &typed) {
          // The name people read; the stored name is that, lower case with underscores.
          std::string key;
          for (char ch : typed)
            key += std::isalnum(static_cast<unsigned char>(ch)) ? char(std::tolower(static_cast<unsigned char>(ch))) : '_';
          while (!key.empty() && key.front() == '_')
            key.erase(key.begin());
          while (!key.empty() && key.back() == '_')
            key.pop_back();
          bool taken = false;
          for (const gen::ExposedInput &e : exposed_in)
            taken = taken || (is_in && e.name == key && e.name != name);
          for (const gen::ExposedOutput &o : exposed_out)
            taken = taken || (!is_in && o.name == key && o.name != name);
          if (key.empty() || taken) {
            say(key.empty() ? "A name is needed." : "There is one with that name.", true);
            return;
          }
          const json *self = of_clip ? clip_json(wf_id_) : nullptr;
          const json workflow_now = self ? object_in(object_in(*self, "media_ref"), "workflow") : *workflow_json();
          json ops = json::array();
          if (is_in) {
            json value = object_in(object_in(object_in(workflow_now, "exposed"), "inputs"), name.c_str());
            if (typed != key)
              value["label"] = typed;
            else
              value.erase("label");
            if (key == name) { // the same name: only its label changes
              ops.push_back({{"op", value.contains("label") ? (input->label.empty() ? "add" : "replace") : "remove"}, {"path", base + "/exposed/inputs/" + name + "/label"}, {"value", value.value("label", std::string())}});
              if (!value.contains("label") && input->label.empty())
                ops.clear();
            } else {
              ops.push_back({{"op", "add"}, {"path", base + "/exposed/inputs/" + key}, {"value", value}});
              ops.push_back({{"op", "remove"}, {"path", base + "/exposed/inputs/" + name}});
              if (of_clip && have.contains(name)) { // the clip's value goes with it
                ops.push_back({{"op", "add"}, {"path", wf_id_ + "/media_ref/inputs/" + key}, {"value", have[name]}});
                ops.push_back({{"op", "remove"}, {"path", wf_id_ + "/media_ref/inputs/" + name}});
              }
            }
          } else if (key != name) {
            const json from = object_in(object_in(object_in(workflow_now, "exposed"), "outputs"), name.c_str());
            ops.push_back({{"op", "add"}, {"path", base + "/exposed/outputs/" + key}, {"value", from}});
            ops.push_back({{"op", "remove"}, {"path", base + "/exposed/outputs/" + name}});
            if (gen::primary_output(workflow_now) == name)
              ops.push_back({{"op", "replace"}, {"path", base + "/exposed/primary"}, {"value", key}});
            if (of_clip && self) { // what the Takes made is kept under the new name
              const json &takes = object_in(object_in(*self, "media_ref"), "takes");
              for (auto t = takes.begin(); t != takes.end(); ++t)
                if (object_in(*t, "outputs").contains(name)) {
                  ops.push_back({{"op", "add"}, {"path", t.key() + "/outputs/" + key}, {"value", (*t)["outputs"][name]}});
                  ops.push_back({{"op", "remove"}, {"path", t.key() + "/outputs/" + name}});
                }
            }
          }
          (void)output;
          if (ops.empty())
            return;
          pending_ = [this, ops, key, is_in] {
            if (patch(ops, "Rename")) {
              wf_row_ = std::string(is_in ? "in:" : "out:") + key;
              wf_text_.erase("row:" + wf_row_);
            }
          };
        });
        ui_mark("field:wf_row_name");
        ImGui::PushTextWrapPos(0.0f);
        if (input) {
          ImGui::TextColored(hexv(look::fg3), "%s%s%s", gen::port_type_name(input->type), input->required ? ", needed" : "",
                             input->to.empty() ? ", feeds nothing: its value is kept" : "");
          if (of_clip && have.contains(name))
            ImGui::TextColored(hexv(look::fg3), "The clip's value is kept when it is renamed.");
        } else {
          ImGui::TextColored(hexv(look::fg3), "%s%s", output->name == primary ? "The Primary Output: the clip plays it." : "The clip gets it.",
                             "");
        }
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        if (input) { // place: the order of the rows on the Clip Inputs node and on the Workflow card
          if (soft_button("wf_row_up", "Move up", ImVec2(92.0f, 28.0f), at > 0))
            pending_ = [this, base, rows = exposed_in, at] {
              std::vector<std::string> order;
              for (const gen::ExposedInput &e : rows)
                order.push_back(e.name);
              std::swap(order[at], order[at - 1]);
              json ops = json::array();
              for (size_t i = 0; i < order.size(); ++i) {
                const gen::ExposedInput *e = nullptr;
                for (const gen::ExposedInput &r : rows)
                  if (r.name == order[i])
                    e = &r;
                const json &stored = object_in(object_in(object_in(*workflow_json(), "exposed"), "inputs"), order[i].c_str());
                if (!stored.contains("order") || stored["order"] != int64_t(i))
                  ops.push_back({{"op", stored.contains("order") ? "replace" : "add"}, {"path", base + "/exposed/inputs/" + order[i] + "/order"}, {"value", int64_t(i)}});
                (void)e;
              }
              patch(ops, "Move input");
            };
          ui_mark("button:wf_row_up");
          ImGui::SameLine();
          if (soft_button("wf_row_down", "Move down", ImVec2(92.0f, 28.0f), at + 1 < exposed_in.size()))
            pending_ = [this, base, rows = exposed_in, at] {
              std::vector<std::string> order;
              for (const gen::ExposedInput &e : rows)
                order.push_back(e.name);
              std::swap(order[at], order[at + 1]);
              json ops = json::array();
              for (size_t i = 0; i < order.size(); ++i) {
                const json &stored = object_in(object_in(object_in(*workflow_json(), "exposed"), "inputs"), order[i].c_str());
                if (!stored.contains("order") || stored["order"] != int64_t(i))
                  ops.push_back({{"op", stored.contains("order") ? "replace" : "add"}, {"path", base + "/exposed/inputs/" + order[i] + "/order"}, {"value", int64_t(i)}});
              }
              patch(ops, "Move input");
            };
          ui_mark("button:wf_row_down");
        } else if (output->name != primary) {
          if (soft_button("wf_row_main", "Make main", ImVec2(110.0f, 28.0f), true, true))
            pending_ = [this, base, name] {
              patch(json::array({{{"op", gen::primary_output(*workflow_json()).empty() ? "add" : "replace"}, {"path", base + "/exposed/primary"}, {"value", name}}}), "Make main output");
            };
          ui_mark("button:wf_row_main");
        }
      }
      end_card();
      if (soft_button("wf_row_remove", is_in ? "Remove this input" : "Remove this output", ImVec2(-1.0f, 30.0f)))
        pending_ = [this, base, name, is_in, of_clip, has_value = have.contains(name), exposed_out] {
          json ops = json::array();
          if (is_in) {
            if (of_clip && has_value)
              ops.push_back({{"op", "remove"}, {"path", wf_id_ + "/media_ref/inputs/" + name}});
            ops.push_back({{"op", "remove"}, {"path", base + "/exposed/inputs/" + name}});
          } else {
            ops.push_back({{"op", "remove"}, {"path", base + "/exposed/outputs/" + name}});
            std::vector<std::string> after;
            for (const gen::ExposedOutput &o : exposed_out)
              if (o.name != name)
                after.push_back(o.name);
            for (json &op : primary_ops(after, gen::primary_output(*workflow_json())))
              ops.push_back(std::move(op));
          }
          if (patch(ops, is_in ? "Remove input" : "Remove output"))
            wf_row_.clear();
        };
      ui_mark("button:wf_row_remove");
      return;
    }
  }

  if (wf_sel_.size() > 1) {
    if (begin_card("##wf_multi", (std::to_string(wf_sel_.size()) + " nodes selected").c_str())) {
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextColored(hexv(look::fg3), "They move and are removed together. Ctrl+C copies them with the links among them, Ctrl+V pastes, Ctrl+D copies and pastes at once.");
      ImGui::PopTextWrapPos();
      ImGui::Dummy(ImVec2(0.0f, 4.0f));
      if (soft_button("wf_copy", "Copy", ImVec2(84.0f, 28.0f)))
        pending_ = [this] { wf_copy(); };
      ImGui::SameLine();
      if (soft_button("wf_duplicate", "Duplicate", ImVec2(96.0f, 28.0f)))
        pending_ = [this] {
          wf_copy();
          wf_pasted_ = 0;
          wf_paste(36.0f);
        };
    }
    end_card();
    if (soft_button("wf_remove_nodes", "Remove these nodes", ImVec2(-1.0f, 30.0f)))
      pending_ = [this] { delete_in_workflow(); };
    return;
  }
  if (!nodes.contains(wf_node_)) { // the workflow itself
    wf_node_.clear();
    if (begin_card("##wf_card", "Workflow")) {
      ImGui::TextColored(hexv(look::fg2), "Name");
      ImGui::SameLine(label_w);
      text_field("name:" + wf_id_, workflow.value("name", std::string()), -1.0f, [this, base, &workflow](const std::string &v) {
        const bool had = workflow.contains("name");
        pending_ = [this, base, v, had] { patch(json::array({{{"op", had ? "replace" : "add"}, {"path", base + "/name"}, {"value", v}}}), "Rename workflow"); };
      });
      ui_mark("field:workflow_name");
      ImGui::PushTextWrapPos(0.0f);
      if (of_clip) {
        const std::string source = workflow.value("source", std::string());
        ImGui::TextColored(hexv(look::fg2), "The workflow of the clip %s%s.", clip ? clip->name.c_str() : wf_id_.c_str(),
                           source.empty() ? "" : (", copied from " + readable_source(gen_models_, source)).c_str());
        if (ImGui::IsItemHovered() && !source.empty())
          ImGui::SetTooltip("%s", source.c_str());
        ImGui::TextColored(hexv(look::fg3), "It is this clip's own: changing it changes no other clip.");
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        workflow_library_buttons(wf_id_, source);
      } else {
        ImGui::TextColored(hexv(look::fg3), "A workflow of the project's library.");
      }
      ImGui::PopTextWrapPos();
    }
    end_card();
    if (begin_card("##wf_face", "Clip inputs and output")) {
      ImGui::PushTextWrapPos(0.0f);
      std::string in, out, unlinked;
      for (const gen::ExposedInput &e : exposed_in) {
        (e.to.empty() ? unlinked : in) += ((e.to.empty() ? unlinked : in).empty() ? "" : ", ") + e.name;
      }
      for (const gen::ExposedOutput &o : exposed_out)
        out += (out.empty() ? "" : ", ") + o.name + (o.name == primary ? " (main)" : "");
      ImGui::TextColored(hexv(look::fg2), "Sets: %s", in.empty() ? "nothing" : in.c_str());
      if (!unlinked.empty())
        ImGui::TextColored(hexv(look::fg3), "Not used by the workflow: %s", unlinked.c_str());
      ImGui::TextColored(hexv(look::fg2), "Gets: %s", out.empty() ? "nothing" : out.c_str());
      ImGui::TextColored(hexv(look::fg3), "Select a node to choose its model and settings, and to see what feeds its inputs.");
      ImGui::PopTextWrapPos();
    }
    end_card();
    if (!of_clip && soft_button("wf_delete", "Delete this workflow", ImVec2(-1.0f, 30.0f)))
      pending_ = [this] {
        const std::string id = wf_id_;
        patch(json::array({{{"op", "remove"}, {"path", id}}}), "Delete workflow");
      };
    return;
  }

  // ---- a node ----
  const std::string id = wf_node_;
  const json &node = nodes[id];
  const std::string kind = node.value("kind", std::string()), short_kind = kind_id(kind);
  const gen::KindDef *def = gen::find_kind(kind);
  const gen::Ports ports = gen::node_ports(library, node);
  if (begin_card("##wf_node", kind_title(kind).c_str())) {
    if (def && def->runs_model) {
      // The models that run this kind of node. A new model brings its own settings: they start at its defaults.
      const std::string model = node.value("model", std::string());
      const json models = wf_parts_.value("models", json::array());
      std::string shown = model.empty() ? "Choose a model" : model;
      const json *decl = nullptr;
      for (const json &m : models)
        if (m.value("id", "") == model) {
          shown = m.value("title", model);
          decl = &m;
        }
      ImGui::TextColored(hexv(look::fg2), "Model");
      ImGui::SameLine(label_w);
      ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
      ImGui::SetNextItemWidth(-1.0f);
      if (ImGui::BeginCombo("##wf_model", shown.c_str())) {
        for (const json &m : models) {
          const json kinds = m.value("kinds", json::array());
          if (std::find(kinds.begin(), kinds.end(), short_kind) == kinds.end())
            continue;
          const std::string mid = m.value("id", "");
          const std::string label = m.value("title", mid) + (m.value("installed", false) ? "" : "  (not installed)") + "##" + mid;
          if (ImGui::Selectable(label.c_str(), mid == model) && mid != model) {
            json settings = json::object();
            for (const json &s : m.value("settings", json::array()))
              if (!s["default"].is_null())
                settings[s.value("name", "")] = s["default"];
            const bool had_model = node.contains("model"), had_settings = node.contains("settings");
            pending_ = [this, id, mid, settings, had_model, had_settings] {
              patch(json::array({{{"op", had_model ? "replace" : "add"}, {"path", id + "/model"}, {"value", mid}},
                                 {{"op", had_settings ? "replace" : "add"}, {"path", id + "/settings"}, {"value", settings}}}),
                    "Change model");
            };
          }
          ui_mark("wfmodel:" + mid);
        }
        ImGui::EndCombo();
      }
      ui_mark("combo:wf_model");
      ImGui::PopStyleColor();
      if (decl && !decl->value("installed", false))
        ImGui::TextColored(kError, "Not installed. Download it in the Models panel.");
      else if (decl && !object_in(*decl, "engines").value(short_kind, false))
        ImGui::TextColored(kError, "Nothing here runs it as this node yet.");

      // Its settings, as the model declares them: nothing here is known in advance.
      const json &have = object_in(node, "settings");
      const bool had_settings = node.contains("settings");
      const auto set = [this, id, had_settings, &have](const std::string &name, json value) {
        const bool had = have.contains(name);
        pending_ = [this, id, name, value, had, had_settings] {
          if (!had_settings)
            patch(json::array({{{"op", "add"}, {"path", id + "/settings"}, {"value", json{{name, value}}}}}), ("Change " + name).c_str());
          else
            patch(json::array({{{"op", had ? "replace" : "add"}, {"path", id + "/settings/" + name}, {"value", value}}}), ("Change " + name).c_str());
        };
      };
      for (const json &s : decl ? decl->value("settings", json::array()) : json::array()) {
        const std::string name = s.value("name", ""), type = s.value("type", "");
        const json now = have.contains(name) ? have[name] : s["default"];
        ImGui::TextColored(hexv(look::fg2), "%s", pretty_name(name).c_str());
        ImGui::SameLine(label_w);
        const std::string key = "set:" + id + "/" + name;
        if (type == "integer" || type == "number") {
          float &v = wf_value_[key];
          if (wf_editing_ != key)
            v = now.is_number() ? now.get<float>() : 0.0f;
          const float lo = s.value("min", 0.0f), hi = s.value("max", 1.0f);
          slim_slider(("wfset_" + name).c_str(), &v, lo, hi > lo ? hi : lo + 1.0f, ImGui::GetContentRegionAvail().x - 56.0f, "");
          if (ImGui::IsItemActive())
            wf_editing_ = key;
          else if (wf_editing_ == key)
            wf_editing_.clear();
          if (type == "integer")
            v = std::round(v);
          if (ImGui::IsItemDeactivatedAfterEdit())
            set(name, type == "integer" ? json(int64_t(std::llround(v))) : json(std::round(double(v) * 1000.0) / 1000.0));
          ImGui::SameLine();
          ImGui::PushFont(g_fonts.mono, 13.0f);
          if (type == "integer")
            ImGui::TextColored(hexv(look::fg2), "%.0f", v);
          else
            ImGui::TextColored(hexv(look::fg2), "%.3f", v);
          ImGui::PopFont();
        } else if (type == "choice") {
          const std::string current = now.is_string() ? now.get<std::string>() : std::string();
          ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
          ImGui::SetNextItemWidth(-1.0f);
          if (ImGui::BeginCombo(("##wfset_" + name).c_str(), current.c_str())) {
            for (const json &option : s.value("options", json::array()))
              if (option.is_string() && ImGui::Selectable(option.get<std::string>().c_str(), option == now) && option != now)
                set(name, option);
            ImGui::EndCombo();
          }
          ui_mark("combo:wfset_" + name);
          ImGui::PopStyleColor();
        } else if (type == "boolean") {
          bool on = now.is_boolean() && now.get<bool>();
          if (ImGui::Checkbox(("##wfset_" + name).c_str(), &on))
            set(name, on);
        } else {
          text_field(key, now.is_string() ? now.get<std::string>() : std::string(), -1.0f, [&](const std::string &v) { set(name, v); });
        }
      }
    } else if (gen::is_workflow_kind(kind)) {
      const std::string inner = node.value("workflow", std::string());
      ImGui::TextColored(hexv(look::fg2), "Runs the workflow \"%s\".", library.contains(inner) ? library[inner].value("name", inner).c_str() : inner.c_str());
      if (library.contains(inner) && soft_button("wf_open_inner", "Open it", ImVec2(96.0f, 28.0f)))
        pending_ = [this, inner] { open_workflow(inner); };
      ui_mark("button:wf_open_inner");
    } else if (short_kind == "variable") {
      // The Variable it reads: one of the project's, by name. Its output has the Variable's Data Type.
      const json &vars = object_in(doc_, "variables");
      const std::string chosen = node.value("variable", std::string());
      ImGui::TextColored(hexv(look::fg2), "Variable");
      ImGui::SameLine(label_w);
      ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
      ImGui::SetNextItemWidth(-1.0f);
      if (ImGui::BeginCombo("##wf_variable", vars.contains(chosen) ? vars[chosen].value("name", chosen).c_str() : "Choose a variable")) {
        for (auto v = vars.begin(); v != vars.end(); ++v) {
          const std::string vid = v.key(), type = v->value("type", std::string("text"));
          if (ImGui::Selectable((v->value("name", vid) + "  (" + type + ")##" + vid).c_str(), vid == chosen) && vid != chosen) {
            const bool had = node.contains("variable"), had_type = node.contains("type");
            pending_ = [this, id, vid, type, had, had_type] {
              patch(json::array({{{"op", had ? "replace" : "add"}, {"path", id + "/variable"}, {"value", vid}},
                                 {{"op", had_type ? "replace" : "add"}, {"path", id + "/type"}, {"value", type}}}),
                    "Choose variable");
            };
          }
          ui_mark("wfvariable:" + v->value("name", vid));
        }
        ImGui::EndCombo();
      }
      ui_mark("combo:wf_variable");
      ImGui::PopStyleColor();
      if (vars.empty())
        ImGui::TextColored(hexv(look::fg3), "The project has no variables yet.");
    } else if (short_kind == "clip_reference") {
      // The clip it reads: the one before or after this clip on the timeline, or a named clip.
      const std::string named = object_in(node, "settings").value("clip", std::string());
      const ClipUi *other = named.empty() || named == "previous" || named == "next" ? nullptr : find_clip(named);
      const std::string shown = named.empty() ? "Choose a clip" : named == "previous" ? "The clip before" : named == "next" ? "The clip after" : other ? other->name : named;
      ImGui::TextColored(hexv(look::fg2), "Clip");
      ImGui::SameLine(label_w);
      ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
      ImGui::SetNextItemWidth(-1.0f);
      if (ImGui::BeginCombo("##wf_reference", shown.c_str())) {
        const auto choose = [&](const std::string &value, const std::string &label, const std::string &mark) {
          if (ImGui::Selectable((label + "##" + value).c_str(), value == named) && value != named) {
            const bool had_settings = node.contains("settings"), had_clip = object_in(node, "settings").contains("clip");
            pending_ = [this, id, value, had_settings, had_clip] {
              if (!had_settings)
                patch(json::array({{{"op", "add"}, {"path", id + "/settings"}, {"value", json{{"clip", value}}}}}), "Choose clip");
              else
                patch(json::array({{{"op", had_clip ? "replace" : "add"}, {"path", id + "/settings/clip"}, {"value", value}}}), "Choose clip");
            };
          }
          ui_mark("wfreference:" + mark);
        };
        choose("previous", "The clip before", "previous");
        choose("next", "The clip after", "next");
        for (const TrackUi &t : tracks_)
          for (const ClipUi &k : t.clips)
            if (k.is_generative && k.id != wf_id_)
              choose(k.id, k.name, k.name);
        ImGui::EndCombo();
      }
      ui_mark("combo:wf_reference");
      ImGui::PopStyleColor();
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextColored(hexv(look::fg3), "Gives that clip's video and sound. This clip is made after it.");
      ImGui::PopTextWrapPos();
    } else if (short_kind == "get_frame") {
      const std::string frame = object_in(node, "settings").value("frame", std::string("last"));
      ImGui::TextColored(hexv(look::fg2), "Frame");
      ImGui::SameLine(label_w);
      ImGui::PushStyleColor(ImGuiCol_FrameBg, hexv(look::raised));
      ImGui::SetNextItemWidth(-1.0f);
      if (ImGui::BeginCombo("##wf_frame", frame == "first" ? "The first frame" : "The last frame")) {
        for (const char *value : {"first", "last"})
          if (ImGui::Selectable(std::string(value) == "first" ? "The first frame" : "The last frame", frame == value) && frame != value) {
            const bool had_settings = node.contains("settings"), had_frame = object_in(node, "settings").contains("frame");
            const std::string chosen = value;
            pending_ = [this, id, chosen, had_settings, had_frame] {
              if (!had_settings)
                patch(json::array({{{"op", "add"}, {"path", id + "/settings"}, {"value", json{{"frame", chosen}}}}}), "Choose frame");
              else
                patch(json::array({{{"op", had_frame ? "replace" : "add"}, {"path", id + "/settings/frame"}, {"value", chosen}}}), "Choose frame");
            };
          }
        ImGui::EndCombo();
      }
      ui_mark("combo:wf_frame");
      ImGui::PopStyleColor();
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextColored(hexv(look::fg3), "Join a time (seconds) to \"at\" to take the frame at that time instead.");
      ImGui::PopTextWrapPos();
    } else if (short_kind == "project") {
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextColored(hexv(look::fg3), "Gives the canvas width and height and the frame rate of the sequence. Nothing about them is held in the workflow.");
      ImGui::PopTextWrapPos();
    } else if (short_kind == "clip") {
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextColored(hexv(look::fg3), "Gives the length of this clip (duration) and where it starts. Change the length of the clip and everything that reads it follows.");
      ImGui::PopTextWrapPos();
    }
  }
  end_card();

  // Its inputs. One that is joined says to what, with a button that breaks the connection; one that is not takes a value
  // typed here, of the input's own type. Joining is done in the graph, by dragging the dots.
  if (begin_card("##wf_inputs", "Inputs")) {
    for (const gen::Port &port : ports.inputs) {
      std::string link_id, from_node, from_port, a, b;
      const gen::ExposedInput *feeder = nullptr; // the Exposed Input that feeds it
      for (auto l = links.begin(); l != links.end(); ++l)
        if (l->contains("to") && end_of((*l)["to"], a, b) && a == id && b == port.name && l->contains("from") && end_of((*l)["from"], from_node, from_port))
          link_id = l.key();
      for (const gen::ExposedInput &e : exposed_in)
        for (const auto &[n, p] : e.to)
          if (n == id && p == port.name)
            feeder = &e;
      const std::string exposed_as = feeder ? feeder->name : std::string();
      const json &typed = object_in(node, "inputs");
      const bool has_value = typed.contains(port.name), linked = !link_id.empty(), by_clip = !exposed_as.empty();
      const bool plain = port.type == gen::PortType::text || port.type == gen::PortType::number || port.type == gen::PortType::integer ||
                         port.type == gen::PortType::boolean;
      const bool quiet = !port.required && !linked && !by_clip && !has_value; // optional and unused: in the background
      ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(ImGui::GetCursorScreenPos().x + 5.0f, ImGui::GetCursorScreenPos().y + 9.0f), 4.5f,
                                                  port_colour(port.type, quiet ? 120 : 255));
      ImGui::Dummy(ImVec2(12.0f, 0.0f));
      ImGui::SameLine();
      ImGui::TextColored(hexv(quiet ? look::fg3 : look::fg), "%s", port.name.c_str());
      ImGui::SameLine();
      ImGui::TextColored(hexv(look::fg3), "%s", gen::port_type_name(port.type));
      if (quiet && !plain)
        continue; // an optional picture or sound with nothing joined: its name is enough
      ImGui::Dummy(ImVec2(12.0f, 0.0f));
      ImGui::SameLine();
      if (linked || by_clip) {
        const std::string source = linked ? (nodes.contains(from_node) ? kind_title(nodes[from_node].value("kind", std::string())) : from_node) + " . " + from_port
                                          : "the clip, as \"" + exposed_as + "\"";
        const float button_w = 92.0f;
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - button_w - 8.0f);
        ImGui::TextColored(hexv(look::fg2), "from %s", source.c_str());
        ImGui::PopTextWrapPos();
        ImGui::SameLine(ImGui::GetWindowWidth() - button_w - 28.0f);
        if (soft_button(("wf_cut_" + port.name).c_str(), "Disconnect", ImVec2(button_w, 24.0f))) {
          json ops = json::array();
          if (linked) {
            ops.push_back({{"op", "remove"}, {"path", link_id}});
          } else { // the Exposed Input stays, and feeds what it still feeds: with nothing left, it is unlinked and dimmed
            json kept = json::array();
            for (const auto &[n, p] : feeder->to)
              if (!(n == id && p == port.name))
                kept.push_back(json::array({n, p}));
            ops.push_back({{"op", "replace"}, {"path", base + "/exposed/inputs/" + exposed_as + "/to"}, {"value", std::move(kept)}});
          }
          pending_ = [this, ops] { patch(ops, "Disconnect"); };
        }
        continue;
      }
      if (!plain) { // pictures, sound, conditioning, latents: they come through a connection only
        ImGui::TextColored(kError, "nothing joined yet: drag a wire to it in the graph");
        continue;
      }
      const std::string key = "in:" + id + "/" + port.name;
      const auto put = [this, id, name = port.name, has_value, has_inputs = node.contains("inputs")](json value) {
        pending_ = [this, id, name, value, has_value, has_inputs] {
          if (!has_inputs)
            patch(json::array({{{"op", "add"}, {"path", id + "/inputs"}, {"value", json{{name, value}}}}}), ("Set " + name).c_str());
          else
            patch(json::array({{{"op", has_value ? "replace" : "add"}, {"path", id + "/inputs/" + name}, {"value", value}}}), ("Set " + name).c_str());
        };
      };
      const float clear_w = has_value ? 64.0f : 0.0f;
      if (port.type == gen::PortType::boolean) { // yes or no: two buttons, the one that holds lit
        const bool on = has_value && typed[port.name].is_boolean() && typed[port.name].get<bool>();
        if (soft_button(("wf_yes_" + port.name).c_str(), "Yes", ImVec2(56.0f, 24.0f), true, has_value && on) && !(has_value && on))
          put(true);
        ImGui::SameLine(0.0f, 6.0f);
        if (soft_button(("wf_no_" + port.name).c_str(), "No", ImVec2(56.0f, 24.0f), true, has_value && !on) && !(has_value && !on))
          put(false);
      } else {
        const std::string current = !has_value ? std::string() : typed[port.name].is_string() ? typed[port.name].get<std::string>() : typed[port.name].dump();
        text_field(key, current, ImGui::GetContentRegionAvail().x - clear_w, [&](const std::string &v) {
          if (port.type == gen::PortType::text)
            return put(v);
          char *end = nullptr;
          const double number = std::strtod(v.c_str(), &end);
          if (v.empty() || end == v.c_str() || *end != 0)
            return say("\"" + v + "\" is not a number.", true);
          put(port.type == gen::PortType::integer ? json(int64_t(std::llround(number))) : json(number));
        });
        ui_mark("field:wfin_" + port.name);
        if (!has_value && !ImGui::IsItemActive() && wf_text_[key][0] == 0) // what goes here, while it is empty
          ImGui::GetWindowDrawList()->AddText(ImVec2(ImGui::GetItemRectMin().x + 8.0f, ImGui::GetItemRectMin().y + 3.0f), hex(look::fg3),
                                              port.type == gen::PortType::text ? "text" : port.type == gen::PortType::integer ? "a whole number" : "a number");
      }
      if (has_value) {
        ImGui::SameLine();
        if (soft_button(("wf_clear_" + port.name).c_str(), "Clear", ImVec2(56.0f, 24.0f)))
          pending_ = [this, id, name = port.name] { patch(json::array({{{"op", "remove"}, {"path", id + "/inputs/" + name}}}), ("Clear " + name).c_str()); };
      }
    }
    if (ports.inputs.empty())
      ImGui::TextColored(hexv(look::fg3), "It takes nothing.");
  }
  end_card();

  if (begin_card("##wf_outputs", "Outputs")) {
    for (const gen::Port &port : ports.outputs) {
      std::string exposed_as;
      for (const gen::ExposedOutput &o : exposed_out)
        if (o.node == id && o.port == port.name)
          exposed_as = o.name;
      ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(ImGui::GetCursorScreenPos().x + 5.0f, ImGui::GetCursorScreenPos().y + 9.0f), 4.5f, port_colour(port.type));
      ImGui::Dummy(ImVec2(12.0f, 0.0f));
      ImGui::SameLine();
      ImGui::TextColored(hexv(look::fg), "%s", port.name.c_str());
      ImGui::SameLine();
      ImGui::TextColored(hexv(look::fg3), "%s", gen::port_type_name(port.type));
      if (exposed_as.empty())
        continue;
      const float button_w = 92.0f;
      ImGui::Dummy(ImVec2(12.0f, 0.0f));
      ImGui::SameLine();
      ImGui::TextColored(hexv(look::fg2), "to the clip, as \"%s\"%s", exposed_as.c_str(), exposed_as == primary ? " (main)" : "");
      ImGui::SameLine(ImGui::GetWindowWidth() - button_w - 28.0f);
      if (soft_button(("wf_cutout_" + port.name).c_str(), "Disconnect", ImVec2(button_w, 24.0f))) {
        json ops = json::array({{{"op", "remove"}, {"path", base + "/exposed/outputs/" + exposed_as}}});
        std::vector<std::string> after;
        for (const gen::ExposedOutput &o : exposed_out)
          if (o.name != exposed_as)
            after.push_back(o.name);
        for (json &op : primary_ops(after, primary))
          ops.push_back(std::move(op));
        pending_ = [this, ops] { patch(ops, "Disconnect"); };
      }
    }
    if (ports.outputs.empty())
      ImGui::TextColored(hexv(look::fg3), "It makes nothing.");
  }
  end_card();
  if (soft_button("wf_remove_node", "Remove this node", ImVec2(-1.0f, 30.0f)))
    pending_ = [this] { delete_in_workflow(); };
}


} // namespace atm::editor

