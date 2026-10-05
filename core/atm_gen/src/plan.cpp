#include "atm/gen/plan.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace atm::gen {
namespace {

const json &object_at(const json &owner, const char *key) {
  static const json empty = json::object();
  if (!owner.is_object())
    return empty;
  const auto it = owner.find(key);
  return it != owner.end() && it->is_object() ? *it : empty;
}

std::string string_at(const json &owner, const char *key) {
  if (!owner.is_object())
    return {};
  const auto it = owner.find(key);
  return it != owner.end() && it->is_string() ? it->get<std::string>() : std::string();
}

const json *selected_take(const json &ref) {
  const std::string id = string_at(ref, "selected");
  const json &takes = object_at(ref, "takes");
  const auto it = takes.find(id);
  return id.empty() || it == takes.end() ? nullptr : &*it;
}

struct Planner {
  const json &library;
  const KeyContext &context;
  const PlanOptions &options;
  std::map<std::string, const ClipIn *> by_id;
  std::map<std::string, ClipPlan> plans;
  std::set<std::string> walking;
  std::map<std::string, KeyContext> contexts;                  // per clip: the context its Input nodes read
  std::map<std::string, std::vector<std::string>> taken_from;  // per clip: the clips its Clip Reference nodes took from
  std::map<std::string, std::map<std::string, Made>> given;    // per clip: what its Clip Reference nodes gave

  // Does the clip make an output of this name?
  bool makes(const std::string &clip, const std::string &output) {
    for (const ExposedOutput &o : exposed_outputs(object_at(*by_id[clip]->ref, "workflow")))
      if (o.name == output)
        return true;
    return false;
  }

  // The context of one clip: what the project gives, what the clip is, and the other clips its references name.
  const KeyContext &context_of(const std::string &id) {
    const auto [it, fresh] = contexts.try_emplace(id, context);
    if (!fresh)
      return it->second;
    KeyContext &c = it->second;
    c.clip = by_id[id]->facts;
    c.reference = [this, id](std::string_view reference, std::string_view port) -> Made {
      const ClipIn &me = *by_id[id];
      std::string target(reference);
      if (target == "previous")
        target = me.previous;
      else if (target == "next")
        target = me.next;
      Made none;
      none.inlined = true; // there is no such clip: the node gives no value
      if (target.empty() || !by_id.contains(target))
        return none;
      std::vector<std::string> &taken = taken_from[id];
      if (std::find(taken.begin(), taken.end(), target) == taken.end())
        taken.push_back(target);
      if (!resolve(target)) // a loop of clips
        return {};
      const Made made = makes(target, std::string(port)) ? made_by_clip(target, std::string(port)) : none;
      if (!made.inlined && made.key.empty())
        return {};
      given[id][std::string(reference) + "|" + std::string(port)] = made;
      return made;
    };
    return c;
  }

  // What an output of a clip is made by. A locked clip gives what its pinned Take recorded; any other clip gives what
  // its inputs ask for now, whether or not that has been generated yet.
  Made made_by_clip(const std::string &clip, const std::string &output) {
    const ClipPlan *p = resolve(clip);
    if (!p)
      return {};
    const ClipIn &in = *by_id[clip];
    if (in.ref->value("locked", false)) {
      const json *take = selected_take(*in.ref);
      const json &out = take ? object_at(object_at(*take, "outputs"), output.c_str()) : object_at(json(), "");
      return Made{string_at(out, "key"), string_at(out, "port")};
    }
    return output_key(library, object_at(*in.ref, "workflow"), p->inputs, output, context_of(clip));
  }

  const ClipPlan *resolve(const std::string &id) {
    if (const auto known = plans.find(id); known != plans.end())
      return &known->second;
    const auto found = by_id.find(id);
    if (found == by_id.end() || !walking.insert(id).second)
      return nullptr; // not a generative clip, or a loop of clips
    const ClipIn &in = *found->second;
    ClipPlan p;
    p.id = in.id;
    p.name = in.name;
    p.source = string_at(object_at(*in.ref, "workflow"), "source");
    const json &written = object_at(*in.ref, "inputs");
    p.inputs = written;
    p.key = take_key(library, object_at(*in.ref, "workflow"), p.inputs, context_of(id)); // reads the other clips it references
    p.depends = taken_from[id];
    p.references = given[id];

    const json *take = selected_take(*in.ref);
    const bool there = take && (!options.present || options.present(*take));
    if (in.ref->value("locked", false)) {
      p.state = ClipState::locked;
      p.out_of_step = !take || string_at(*take, "key") != p.key;
    } else if (!take) {
      p.state = ClipState::empty;
    } else if (!p.key.empty() && string_at(*take, "key") == p.key && there) {
      p.state = ClipState::clean;
    } else {
      p.state = ClipState::dirty;
      if (!there) {
        p.reason = "the files of its Take are gone";
      } else { // which of its own inputs moved since the Take; none: something it depends on did
        const json &then = object_at(*take, "inputs");
        std::string changed;
        for (auto it = written.begin(); it != written.end(); ++it)
          if (!then.contains(it.key()) || then[it.key()] != *it)
            changed += (changed.empty() ? "" : ", ") + it.key();
        for (auto it = then.begin(); it != then.end(); ++it)
          if (!written.contains(it.key()))
            changed += (changed.empty() ? "" : ", ") + it.key();
        p.reason = !changed.empty()       ? "changed: " + changed
                   : !p.depends.empty()   ? "a clip it takes from, its length, a Variable or its workflow changed"
                                          : "its length, a Variable, the canvas or its workflow changed";
      }
    }
    walking.erase(id);
    return &(plans[id] = std::move(p));
  }
};

} // namespace

const char *clip_state_name(ClipState state) {
  static constexpr const char *kNames[] = {"clean", "dirty", "empty", "locked"};
  return kNames[size_t(state)];
}

std::vector<ClipPlan> plan(const json &library, const std::vector<ClipIn> &clips, const KeyContext &context,
                           const PlanOptions &options) {
  Planner planner{library, context, options, {}, {}, {}};
  std::vector<const ClipIn *> ordered;
  for (const ClipIn &c : clips)
    if (c.ref && c.ref->is_object()) {
      planner.by_id[c.id] = &c;
      ordered.push_back(&c);
    }
  std::stable_sort(ordered.begin(), ordered.end(), [](const ClipIn *a, const ClipIn *b) { return a->order < b->order; });
  for (const ClipIn *c : ordered)
    planner.resolve(c->id);
  std::map<std::string, ClipPlan> &plans = planner.plans;

  // Which clips run.
  const auto needs = [&](const ClipPlan &p) { return p.state == ClipState::dirty || p.state == ClipState::empty; };
  const auto named = [&](const std::string &id) { return std::find(options.clips.begin(), options.clips.end(), id) != options.clips.end(); };
  const bool by_name = options.scope == Scope::selected || options.scope == Scope::selected_and_after;
  for (auto &[id, p] : plans) {
    const bool asked = options.scope == Scope::all || (options.scope == Scope::dirty && needs(p)) || (by_name && named(id));
    if (!asked)
      continue;
    if (p.state == ClipState::locked)
      p.skip = "it is locked";
    else
      p.run = true;
  }
  if (options.scope == Scope::selected_and_after) { // everything that takes from a clip that runs
    for (bool grew = true; grew;) {
      grew = false;
      for (auto &[id, p] : plans)
        if (!p.run && p.state != ClipState::locked)
          for (const std::string &d : p.depends)
            if (const auto up = plans.find(d); up != plans.end() && up->second.run) {
              p.run = grew = true;
              break;
            }
    }
  }
  for (bool grew = true; grew;) { // what a running clip needs upstream that is itself dirty or empty
    grew = false;
    for (auto &[id, p] : plans)
      if (p.run)
        for (const std::string &d : p.depends)
          if (const auto up = plans.find(d); up != plans.end() && !up->second.run && needs(up->second))
            up->second.run = grew = true;
  }
  for (bool changed = true; changed;) { // a clip that cannot get what it starts from does not run
    changed = false;
    for (auto &[id, p] : plans) {
      if (!p.run)
        continue;
      std::string why;
      if (p.key.empty())
        why = "its inputs cannot be worked out";
      for (const std::string &d : p.depends) {
        const auto up = plans.find(d);
        if (up == plans.end())
          why = "it starts from a clip that is not generative";
        else if (up->second.state == ClipState::locked && up->second.key.empty() && !selected_take(*planner.by_id[d]->ref))
          why = "it starts from " + up->second.name + ", which is locked and has no Take";
        else if (!up->second.run && !up->second.skip.empty() && up->second.state != ClipState::locked)
          why = "it starts from " + up->second.name + ", which does not run";
      }
      if (!why.empty()) {
        p.run = false;
        p.skip = why;
        changed = true;
      }
    }
  }

  // In order: a clip after the clips it takes from; otherwise timeline order.
  std::vector<ClipPlan> out;
  std::set<std::string> placed;
  std::function<void(const std::string &)> place = [&](const std::string &id) {
    const auto it = plans.find(id);
    if (it == plans.end() || !placed.insert(id).second)
      return;
    for (const std::string &d : it->second.depends)
      place(d);
    out.push_back(it->second);
  };
  for (const ClipIn *c : ordered)
    place(c->id);
  return out;
}

} // namespace atm::gen
