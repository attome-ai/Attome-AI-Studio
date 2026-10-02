#include "atm/patch/patch.hpp"

#include <algorithm>
#include <array>
#include <set>

#include "atm/base/id.hpp"
#include "atm/base/profiler.hpp"
#include "atm/base/time.hpp"

namespace atm::patch {
namespace {

using doc::NodeRef;
using std::string_view;

constexpr size_t kMaxSegments = 16;
constexpr size_t kMaxOps = 10000;
constexpr size_t kMaxProblems = 20;
constexpr size_t kNone = SIZE_MAX;

struct Path {
  std::array<string_view, kMaxSegments> seg;
  size_t n = 0;
  string_view last() const { return seg[n - 1]; }
};

Result<Path> split(string_view path) {
  Path p;
  size_t start = 0;
  for (;;) {
    const size_t slash = path.find('/', start);
    const string_view part = path.substr(start, slash == string_view::npos ? string_view::npos : slash - start);
    if (part.empty() || p.n == kMaxSegments)
      return fail(ErrorCode::InvalidArgument, "P_PATH", "\"" + std::string(path) + "\" is not a valid path.",
                  std::string(path), "Paths look like \"<id>/<field>\" or \"<parentId>/<collection>/<id>\".");
    p.seg[p.n++] = part;
    if (slash == string_view::npos)
      return p;
    start = slash + 1;
  }
}

std::string join(const Path &p, size_t from, size_t to) {
  std::string out;
  for (size_t i = from; i < to; ++i) {
    if (i > from)
      out += '/';
    out += p.seg[i];
  }
  return out;
}

bool ends_with_order(string_view key) { return key.size() > 6 && key.substr(key.size() - 6) == "_order"; }

bool is_time_key(string_view k) {
  return k == "record_in" || k == "duration" || k == "source_in" || k == "t" || k == "start";
}

bool is_time_like(const json &v) {
  return v.is_string() || v.is_number_integer() || (v.is_object() && v.contains("num"));
}

bool contains_ids(const json &v) {
  if (v.is_object()) {
    for (auto it = v.begin(); it != v.end(); ++it)
      if (is_stable_id(it.key()) || contains_ids(*it))
        return true;
  } else if (v.is_array()) {
    for (const auto &e : v)
      if (contains_ids(e))
        return true;
  }
  return false;
}

size_t index_of(const json &arr, string_view id) {
  for (size_t i = 0; i < arr.size(); ++i)
    if (arr[i].is_string() && arr[i].get_ref<const std::string &>() == id)
      return i;
  return kNone;
}

json *descend(json *node, const Path &p, size_t from, size_t to) {
  for (size_t i = from; i < to; ++i) {
    if (!node->is_object())
      return nullptr;
    const auto it = node->find(p.seg[i]);
    if (it == node->end())
      return nullptr;
    node = &*it;
  }
  return node;
}

json problem(string_view rule, std::string path, string_view target, std::string message, std::string hint) {
  return {{"code", uint32_t(ErrorCode::SchemaViolation)},
          {"rule", rule},
          {"path", std::move(path)},
          {"target", target},
          {"message", std::move(message)},
          {"hint", std::move(hint)}};
}

// Timing rules of one track: every clip has a valid timing, and clips do not overlap.
void check_track(const doc::Document &doc, const std::string &track_id, json &problems) {
  const NodeRef *ref = doc.find(track_id);
  if (!ref)
    return;
  const auto clips = ref->node->find("clips");
  if (clips == ref->node->end() || !clips->is_object())
    return;
  struct Span {
    Rational in, end;
    const std::string *id;
  };
  std::vector<Span> spans;
  spans.reserve(clips->size());
  for (auto it = clips->begin(); it != clips->end() && problems.size() < kMaxProblems; ++it) {
    const std::string &id = it.key();
    const auto timing = it->find("timing");
    const json *in = nullptr, *dur = nullptr;
    if (timing != it->end() && timing->is_object()) {
      if (auto f = timing->find("record_in"); f != timing->end() && f->is_string())
        in = &*f;
      if (auto f = timing->find("duration"); f != timing->end() && f->is_string())
        dur = &*f;
    }
    if (!in || !dur) {
      problems.push_back(problem("R_CLIP_TIMING", id + "/timing", id,
                                 "Clip " + id + " has no timing with record_in and duration.",
                                 "Add \"timing\": {\"record_in\": \"0s\", \"duration\": \"5s\"} to the clip."));
      continue;
    }
    const auto rin = Rational::parse(in->get_ref<const std::string &>());
    const auto rdur = Rational::parse(dur->get_ref<const std::string &>());
    if (!rin || !rdur) {
      problems.push_back(problem("T_PARSE", id + "/timing", id, "Clip " + id + " has a timing value that is not a time.",
                                 "Write times like \"12.5s\" or \"1001/30000\"."));
      continue;
    }
    if (rdur->num() <= 0) {
      problems.push_back(problem("R_CLIP_DURATION", id + "/timing/duration", id,
                                 "Clip " + id + " has a duration of " + rdur->to_string() + "; it must be above zero.",
                                 "Set a positive duration, for example \"1s\"."));
      continue;
    }
    const auto end = add(*rin, *rdur);
    if (!end) {
      problems.push_back(problem("T_OVERFLOW", id + "/timing", id, "Clip " + id + " ends beyond the largest time.",
                                 "Use a smaller record_in or duration."));
      continue;
    }
    spans.push_back({*rin, *end, &id});
  }
  std::sort(spans.begin(), spans.end(), [](const Span &a, const Span &b) { return compare(a.in, b.in) < 0; });
  for (size_t i = 1; i < spans.size() && problems.size() < kMaxProblems; ++i) {
    const Span &a = spans[i - 1], &b = spans[i];
    if (compare(a.end, b.in) <= 0)
      continue;
    problems.push_back(problem(
        "R_TRACK_OVERLAP", *b.id + "/timing", track_id,
        "Clip " + *b.id + " (" + b.in.to_string() + " to " + b.end.to_string() + ") overlaps clip " + *a.id + " (" +
            a.in.to_string() + " to " + a.end.to_string() + ") on track " + track_id + ".",
        "Set " + *b.id + "/timing/record_in to \"" + a.end.to_string() + "\", or shorten " + *a.id + " first."));
  }
}

tl::unexpected<Error> rejected(json problems) {
  Error e;
  e.code = ErrorCode::SchemaViolation;
  const json &first = problems.front();
  e.rule = first.value("rule", "");
  e.path = first.value("path", "");
  e.hint = first.value("hint", "");
  e.message = problems.size() == 1 ? first.value("message", "")
                                   : "Patch rejected: " + std::to_string(problems.size()) + " problems.";
  e.errors = std::move(problems);
  return tl::unexpected(std::move(e));
}

struct Target {
  std::string id;
  json *node = nullptr;
};

class Applier {
public:
  // `internal` runs inverses and journal replay, which may hold the engine-only "prune" op.
  explicit Applier(doc::Document &doc, bool internal = false) : doc_(doc), internal_(internal) {}

  Result<void> run(const json &ops) {
    if (!ops.is_array())
      return fail(ErrorCode::InvalidArgument, "P_OPS", "A patch needs an \"ops\" array.", {},
                  "Send {\"ops\": [{\"op\": \"replace\", \"path\": \"<id>/<field>\", \"value\": …}]}.");
    if (ops.size() > kMaxOps)
      return fail(ErrorCode::InvalidArgument, "P_TOO_LARGE", "A patch may hold at most 10000 ops.", {},
                  "Split the patch into several calls.");
    for (size_t i = 0; i < ops.size(); ++i) {
      auto r = run_op(ops[i]);
      if (!r) {
        for (json &prune : prunes_) // containers the failed op had already created
          inv_.push_back(std::move(prune));
        prunes_.clear();
        Error e = std::move(r.error());
        if (!e.details.is_object())
          e.details = json::object();
        e.details["op_index"] = i;
        return tl::unexpected(std::move(e));
      }
    }
    return {};
  }

  Result<void> validate() {
    std::set<std::string> tracks;
    for (const std::string &id : timing_clips_)
      if (const NodeRef *ref = doc_.find(id); ref && id_prefix(ref->parent) == "trk")
        tracks.insert(ref->parent);
    for (const std::string &id : res_.created) {
      const string_view prefix = id_prefix(id);
      if (prefix == "trk") {
        tracks.insert(id);
      } else if (prefix == "seq") {
        if (const NodeRef *ref = doc_.find(id))
          if (auto t = ref->node->find("tracks"); t != ref->node->end() && t->is_object())
            for (auto it = t->begin(); it != t->end(); ++it)
              tracks.insert(it.key());
      }
    }
    json problems = json::array();
    for (const std::string &t : tracks)
      check_track(doc_, t, problems);
    if (!problems.empty())
      return rejected(std::move(problems));
    return {};
  }

  json inverse() const { // newest first, so applying it in order undoes the patch
    json inv = json::array();
    for (auto it = inv_.rbegin(); it != inv_.rend(); ++it)
      if (!it->is_null())
        inv.push_back(*it);
    return inv;
  }

  ApplyResult take() {
    res_.inverse = inverse();
    std::sort(res_.modified.begin(), res_.modified.end());
    res_.modified.erase(std::unique(res_.modified.begin(), res_.modified.end()), res_.modified.end());
    return std::move(res_);
  }

private:
  Result<void> run_op(const json &op) {
    const auto name_it = op.is_object() ? op.find("op") : op.end();
    const auto path_it = op.is_object() ? op.find("path") : op.end();
    if (!op.is_object() || name_it == op.end() || !name_it->is_string() || path_it == op.end() ||
        !path_it->is_string())
      return fail(ErrorCode::InvalidArgument, "P_OP", "Every op needs string fields \"op\" and \"path\".", {},
                  "Example: {\"op\": \"replace\", \"path\": \"<id>/name\", \"value\": \"Intro\"}.");
    const std::string &name = name_it->get_ref<const std::string &>();
    const std::string &path = path_it->get_ref<const std::string &>();
    ATM_TRY(Path p, split(path));
    Result<void> r;
    if (name == "replace")
      r = op_replace(op, p);
    else if (name == "add")
      r = (p.n >= 3 && (is_placeholder(p.last()) || is_stable_id(p.last()))) ? add_object(op, p) : add_field(op, p);
    else if (name == "remove")
      r = (p.n == 1 || (p.n >= 3 && is_stable_id(p.last()) && doc_.find(p.last()))) ? remove_object(p)
                                                                                    : remove_field(p);
    else if (name == "move")
      r = op_move(op, p);
    else if (name == "insert_order")
      r = insert_order(op, p);
    else if (name == "remove_order")
      r = remove_order(op, p);
    else if (name == "test")
      r = op_test(op, p);
    else if (name == "prune" && internal_)
      r = op_prune(p);
    else
      return fail(ErrorCode::InvalidArgument, "P_OP", "\"" + name + "\" is not a patch op.", path,
                  "Use add, remove, replace, move, insert_order, remove_order or test.");
    if (!r && r.error().path.empty())
      r.error().path = path;
    return r;
  }

  std::string resolve_ref(const std::string &s) const {
    if (is_placeholder(s))
      if (auto it = res_.id_map.find(s); it != res_.id_map.end())
        return it->get<std::string>();
    return s;
  }

  Result<Target> resolve(string_view seg) const {
    const std::string id = resolve_ref(std::string(seg));
    const NodeRef *ref = doc_.find(id);
    if (!ref)
      return fail(ErrorCode::UnknownId, is_placeholder(id) ? "P_PLACEHOLDER" : "P_UNKNOWN_ID",
                  "No object has the ID \"" + id + "\".", {},
                  "List the project's IDs with: attome inspect <project> --level full");
    return Target{id, ref->node};
  }

  TimeContext context_of(const std::string &owner) const {
    for (std::string cur = owner; !cur.empty();) {
      const NodeRef *ref = doc_.find(cur);
      if (!ref)
        break;
      if (id_prefix(cur) == "seq") {
        if (auto r = ref->node->find("rate"); r != ref->node->end() && r->is_string())
          if (auto rate = Rational::parse(r->get_ref<const std::string &>()))
            return TimeContext{*rate};
        break;
      }
      cur = ref->parent;
    }
    return {};
  }

  Result<void> normalize_time(json &v, const std::string &owner) const {
    auto r = parse_time(v);
    if (!r && r.error().rule == "T_CONTEXT") // SMPTE: the rate comes from the owning Sequence
      r = parse_time(v, context_of(owner));
    if (!r)
      return tl::unexpected(std::move(r.error()));
    v = r->to_string();
    return {};
  }

  // Time-typed fields accept every boundary spelling and are stored canonically.
  Result<void> normalize_times(json &v, const std::string &owner) const {
    if (v.is_object()) {
      for (auto it = v.begin(); it != v.end(); ++it) {
        const std::string &key = it.key();
        if (is_time_key(key) && is_time_like(*it)) {
          ATM_CHECK(normalize_time(*it, owner));
        } else if (key == "rate" && it->is_string()) {
          ATM_TRY(Rational rate, Rational::parse(it->get_ref<const std::string &>()));
          *it = rate.to_string();
        } else if (it->is_structured()) {
          ATM_CHECK(normalize_times(*it, owner));
        }
      }
    } else if (v.is_array()) {
      for (auto &e : v)
        if (e.is_structured())
          ATM_CHECK(normalize_times(e, owner));
    }
    return {};
  }

  Result<void> normalize_field(json &v, string_view key, const std::string &owner) const {
    if (is_time_key(key) && is_time_like(v))
      return normalize_time(v, owner);
    if (key == "rate" && v.is_string()) {
      ATM_TRY(Rational rate, Rational::parse(v.get_ref<const std::string &>()));
      v = rate.to_string();
      return {};
    }
    return normalize_times(v, owner);
  }

  void replace_refs(json &v) const {
    if (res_.id_map.empty())
      return;
    if (v.is_string()) {
      const std::string &s = v.get_ref<const std::string &>();
      if (is_placeholder(s))
        if (auto it = res_.id_map.find(s); it != res_.id_map.end())
          v = *it;
    } else if (v.is_structured()) {
      for (auto &e : v)
        replace_refs(e);
    }
  }

  // Give fresh IDs to "$new:<name>" keys nested inside an added value.
  Result<void> mint_keys(json &node, string_view field, bool keyframes) {
    if (!node.is_object())
      return {};
    std::vector<std::string> placeholders;
    for (auto it = node.begin(); it != node.end(); ++it)
      if (is_placeholder(it.key()))
        placeholders.push_back(it.key());
    for (const std::string &k : placeholders) {
      ATM_TRY(std::string id, mint(k, keyframes ? string_view("kf") : doc::prefix_for_collection(field), field));
      json v = std::move(node[k]);
      node.erase(k);
      node[id] = std::move(v);
    }
    for (auto it = node.begin(); it != node.end(); ++it)
      if (it->is_object()) {
        const std::string &key = it.key();
        ATM_CHECK(mint_keys(*it, is_stable_id(key) ? field : string_view(key), keyframes || key == "keyframes"));
      }
    return {};
  }

  Result<std::string> mint(string_view placeholder, string_view prefix, string_view collection) {
    if (prefix.empty())
      return fail(ErrorCode::InvalidArgument, "P_PREFIX",
                  "The collection \"" + std::string(collection) + "\" has no known ID prefix for a placeholder.", {},
                  "Use a full Stable ID instead of \"" + std::string(placeholder) + "\".");
    if (res_.id_map.contains(placeholder))
      return fail(ErrorCode::InvalidArgument, "P_EXISTS",
                  "The placeholder \"" + std::string(placeholder) + "\" is created twice in this patch.", {},
                  "Give each new object its own placeholder name.");
    std::string id = new_id(prefix);
    res_.id_map[std::string(placeholder)] = id;
    return id;
  }

  Result<void> check_fresh(const json &v) const {
    if (!v.is_object())
      return {};
    for (auto it = v.begin(); it != v.end(); ++it) {
      if (is_stable_id(it.key()) && doc_.find(it.key()))
        return fail(ErrorCode::InvalidArgument, "P_EXISTS", "The ID \"" + it.key() + "\" is already in use.", {},
                    "Use a \"$new:<name>\" placeholder to get a fresh ID.");
      ATM_CHECK(check_fresh(*it));
    }
    return {};
  }

  // Position in an order array from an anchor: {"first"}, {"last"} (default), {"before": id}, {"after": id}.
  Result<size_t> anchor_pos(const json *arr, const json &op) const {
    const size_t size = arr && arr->is_array() ? arr->size() : 0;
    const auto a = op.find("anchor");
    if (a == op.end() || !a->is_object())
      return size;
    if (a->value("none", false))
      return kNone;
    if (a->value("first", false))
      return size_t(0);
    if (a->value("last", false))
      return size;
    for (const char *key : {"before", "after"})
      if (auto it = a->find(key); it != a->end() && it->is_string()) {
        const std::string ref = resolve_ref(it->get_ref<const std::string &>());
        const size_t i = size ? index_of(*arr, ref) : kNone;
        if (i == kNone)
          return fail(ErrorCode::InvalidArgument, "P_ANCHOR",
                      "The anchor \"" + ref + "\" is not in the target collection.", {},
                      "Anchor to an ID in the same collection, or use {\"last\": true}.");
        return i + (key[0] == 'a' ? 1 : 0);
      }
    return fail(ErrorCode::InvalidArgument, "P_ANCHOR", "An anchor needs first, last, before or after.", {},
                "Example: \"anchor\": {\"after\": \"<id>\"}.");
  }

  json anchor_json(const json &op) const {
    const auto a = op.find("anchor");
    if (a == op.end() || !a->is_object())
      return nullptr;
    json out = *a;
    for (const char *key : {"before", "after"})
      if (auto it = out.find(key); it != out.end() && it->is_string())
        *it = resolve_ref(it->get_ref<const std::string &>());
    return out;
  }

  static json anchor_at(const json &arr, size_t idx) {
    if (idx == 0)
      return {{"first", true}};
    return {{"after", arr[idx - 1]}};
  }

  // Containers that an op creates on the way (missing parent objects, a first collection map, a first order
  // array) are removed again by its inverse through "prune" ops, so that undo restores the exact document.
  json *ensure(json *node, const Path &p, size_t from, size_t to, const std::string &owner) {
    for (size_t i = from; i < to; ++i) {
      if (!node->is_object())
        return nullptr;
      auto it = node->find(p.seg[i]);
      if (it == node->end()) {
        it = node->emplace(std::string(p.seg[i]), json::object()).first;
        prunes_.push_back({{"op", "prune"}, {"path", owner + "/" + join(p, 1, i + 1)}});
      }
      node = &*it;
    }
    return node;
  }

  json &ensure_key(json &container, const std::string &key, json empty, const std::string &container_path) {
    auto it = container.find(key);
    if (it == container.end()) {
      it = container.emplace(key, std::move(empty)).first;
      prunes_.push_back({{"op", "prune"}, {"path", container_path + "/" + key}});
    }
    return *it;
  }

  void insert_at(json &container, const std::string &order_key, size_t pos, const std::string &id,
                 const std::string &container_path) {
    json &arr = ensure_key(container, order_key, json::array(), container_path);
    if (!arr.is_array())
      arr = json::array();
    arr.insert(arr.begin() + std::ptrdiff_t(std::min(pos, arr.size())), id);
  }

  Result<void> op_prune(const Path &p) {
    ATM_TRY(Target t, resolve(p.seg[0]));
    json *parent = p.n >= 2 ? descend(t.node, p, 1, p.n - 1) : nullptr;
    if (parent && parent->is_object())
      if (const auto it = parent->find(p.last()); it != parent->end() && it->is_structured() && it->empty())
        parent->erase(it);
    return {};
  }

  void note_timing(const std::string &id, const Path &p) {
    if (p.n >= 2 && p.seg[1] == "timing" && id_prefix(id) == "clp")
      timing_clips_.insert(id);
  }

  void done(json forward, json inverse) {
    res_.ops.push_back(std::move(forward));
    for (json &prune : prunes_) // run after the inverse below, innermost first (the inverse list is reversed)
      inv_.push_back(std::move(prune));
    prunes_.clear();
    inv_.push_back(std::move(inverse));
  }

  Result<void> add_object(const json &op, const Path &p) {
    ATM_TRY(Target parent, resolve(p.seg[0]));
    const string_view last = p.last(), coll_name = p.seg[p.n - 2];
    const auto vit = op.find("value");
    if (vit == op.end() || !vit->is_object())
      return fail(ErrorCode::InvalidArgument, "P_VALUE", "Adding an object needs an object \"value\".");
    bool keyframes = false;
    for (size_t i = 1; i + 1 < p.n; ++i)
      keyframes = keyframes || p.seg[i] == "keyframes";
    const string_view prefix = keyframes ? string_view("kf") : doc::prefix_for_collection(coll_name);
    std::string id;
    if (is_placeholder(last)) {
      ATM_TRY(std::string minted, mint(last, prefix, coll_name));
      id = std::move(minted);
    } else {
      id = last;
      if (!prefix.empty() && id_prefix(id) != prefix)
        return fail(ErrorCode::SchemaViolation, "S_ID_PREFIX",
                    "Objects in \"" + std::string(coll_name) + "\" need IDs that start with \"" + std::string(prefix) +
                        "_\".",
                    {}, "Use the placeholder \"$new:<name>\" to get a correct ID.");
      if (doc_.find(id))
        return fail(ErrorCode::InvalidArgument, "P_EXISTS", "The ID \"" + id + "\" is already in use.", {},
                    "Use \"replace\" to change the object, or a \"$new:<name>\" placeholder for a new one.");
    }
    json value = *vit;
    ATM_CHECK(mint_keys(value, coll_name, keyframes));
    replace_refs(value);
    ATM_CHECK(check_fresh(value));
    ATM_CHECK(normalize_times(value, parent.id));

    const std::string coll = join(p, 1, p.n - 1);
    const std::string order_key = doc::order_key_for(coll_name);
    json *container = ensure(parent.node, p, 1, p.n - 2, parent.id); // holds the map and its order array
    if (!container || !container->is_object())
      return fail(ErrorCode::InvalidArgument, "P_PATH", "The path does not lead to a collection.");
    const std::string container_path = p.n > 3 ? parent.id + "/" + join(p, 1, p.n - 2) : parent.id;
    size_t pos = kNone;
    if (!order_key.empty()) {
      const auto order = container->find(order_key);
      ATM_TRY(size_t q, anchor_pos(order == container->end() ? nullptr : &*order, op));
      pos = q;
    }
    json &map = ensure_key(*container, std::string(coll_name), json::object(), container_path);
    if (!map.is_object())
      return fail(ErrorCode::InvalidArgument, "P_PATH", "\"" + coll + "\" is not a collection.");

    json forward = {{"op", "add"}, {"path", parent.id + "/" + coll + "/" + id}, {"value", value}};
    if (json a = anchor_json(op); !a.is_null())
      forward["anchor"] = std::move(a);
    const auto it = map.emplace(id, std::move(value)).first;
    doc_.index_subtree(*it, id, parent.id, coll);
    if (pos != kNone)
      insert_at(*container, order_key, pos, id, container_path);
    if (prefix == "clp")
      timing_clips_.insert(id);
    res_.created.push_back(id);
    done(std::move(forward), {{"op", "remove"}, {"path", id}});
    return {};
  }

  Result<void> add_field(const json &op, const Path &p) {
    ATM_TRY(Target t, resolve(p.seg[0]));
    const auto vit = op.find("value");
    if (p.n < 2 || vit == op.end())
      return fail(ErrorCode::InvalidArgument, "P_PATH", "\"add\" needs a path \"<id>/<field>\" and a \"value\".");
    json value = *vit;
    replace_refs(value);
    if (ends_with_order(p.last()) || contains_ids(value))
      return fail(ErrorCode::InvalidArgument, "P_CHILDREN_FIELD",
                  "Collections and order arrays cannot be set as plain fields.", {},
                  "Add objects with \"<parentId>/<collection>/$new:<name>\" and reorder with \"insert_order\".");
    ATM_CHECK(normalize_field(value, p.last(), t.id));
    json *parent = ensure(t.node, p, 1, p.n - 1, t.id);
    if (!parent || !parent->is_object())
      return fail(ErrorCode::InvalidArgument, "P_PATH", "The path runs through a value that is not an object.");
    if (parent->contains(p.last()))
      return fail(ErrorCode::InvalidArgument, "P_EXISTS", "The field already has a value.", {},
                  "Use \"replace\" to change it.");
    const std::string path = t.id + "/" + join(p, 1, p.n);
    parent->emplace(std::string(p.last()), value);
    res_.modified.push_back(t.id);
    note_timing(t.id, p);
    done({{"op", "add"}, {"path", path}, {"value", std::move(value)}}, {{"op", "remove"}, {"path", path}});
    return {};
  }

  Result<void> op_replace(const json &op, const Path &p) {
    ATM_TRY(Target t, resolve(p.seg[0]));
    const auto vit = op.find("value");
    if (p.n < 2 || vit == op.end())
      return fail(ErrorCode::InvalidArgument, "P_PATH", "\"replace\" needs a path \"<id>/<field>\" and a \"value\".");
    json value = *vit;
    replace_refs(value);
    if (ends_with_order(p.last()) || contains_ids(value))
      return fail(ErrorCode::InvalidArgument, "P_CHILDREN_FIELD",
                  "Collections and order arrays cannot be replaced as plain fields.", {},
                  "Use add / remove / move for objects and insert_order / remove_order for order arrays.");
    ATM_CHECK(normalize_field(value, p.last(), t.id));
    json *parent = ensure(t.node, p, 1, p.n - 1, t.id);
    if (!parent || !parent->is_object())
      return fail(ErrorCode::InvalidArgument, "P_PATH", "The path runs through a value that is not an object.");
    const std::string path = t.id + "/" + join(p, 1, p.n);
    json inverse;
    const auto it = parent->find(p.last());
    if (it == parent->end()) {
      parent->emplace(std::string(p.last()), value);
      inverse = {{"op", "remove"}, {"path", path}};
    } else {
      if (contains_ids(*it))
        return fail(ErrorCode::InvalidArgument, "P_CHILDREN_FIELD", "This field holds child objects.", {},
                    "Change the child objects by their own IDs.");
      inverse = {{"op", "replace"}, {"path", path}, {"value", std::move(*it)}};
      *it = value;
    }
    res_.modified.push_back(t.id);
    note_timing(t.id, p);
    done({{"op", "replace"}, {"path", path}, {"value", std::move(value)}}, std::move(inverse));
    return {};
  }

  struct Home { // where an object lives
    std::string parent, collection, order_key;
    json *container = nullptr, *map = nullptr, *order = nullptr;
    size_t order_index = kNone;
  };

  Result<Home> home_of(const std::string &id) const {
    const NodeRef *ref = doc_.find(id);
    if (ref->parent.empty())
      return fail(ErrorCode::InvalidArgument, "P_ROOT", "The project itself cannot be removed or moved.");
    Home h;
    h.parent = ref->parent;
    h.collection = ref->collection;
    ATM_TRY(Path cp, split(h.collection));
    h.container = descend(doc_.find(h.parent)->node, cp, 0, cp.n - 1);
    h.map = h.container ? descend(h.container, cp, cp.n - 1, cp.n) : nullptr;
    if (!h.map || !h.map->is_object())
      return fail(ErrorCode::Internal, "P_INDEX", "The document index is out of step with the tree.");
    h.order_key = doc::order_key_for(cp.last());
    if (!h.order_key.empty())
      if (auto o = h.container->find(h.order_key); o != h.container->end() && o->is_array()) {
        h.order = &*o;
        h.order_index = index_of(*o, id);
      }
    return h;
  }

  json old_anchor(const Home &h) const {
    if (h.order_key.empty())
      return nullptr;
    return h.order_index == kNone ? json{{"none", true}} : anchor_at(*h.order, h.order_index);
  }

  Result<void> remove_object(const Path &p) {
    ATM_TRY(Target t, resolve(p.last()));
    ATM_TRY(Home h, home_of(t.id));
    json inverse = {{"op", "add"}, {"path", h.parent + "/" + h.collection + "/" + t.id}};
    if (json a = old_anchor(h); !a.is_null())
      inverse["anchor"] = std::move(a);
    if (h.order_index != kNone)
      h.order->erase(h.order_index);
    const auto it = h.map->find(t.id);
    doc_.unindex_subtree(*it, t.id);
    inverse["value"] = std::move(*it);
    h.map->erase(it);
    res_.deleted.push_back(t.id);
    done({{"op", "remove"}, {"path", t.id}}, std::move(inverse));
    return {};
  }

  Result<void> remove_field(const Path &p) {
    ATM_TRY(Target t, resolve(p.seg[0]));
    json *parent = descend(t.node, p, 1, p.n - 1);
    if (!parent || !parent->is_object() || !parent->contains(p.last()))
      return fail(ErrorCode::UnknownId, "P_NO_FIELD", "The field to remove does not exist.");
    const auto it = parent->find(p.last());
    if (ends_with_order(p.last()) || contains_ids(*it))
      return fail(ErrorCode::InvalidArgument, "P_CHILDREN_FIELD", "Collections and order arrays cannot be removed.",
                  {}, "Remove the child objects by their own IDs.");
    const std::string path = t.id + "/" + join(p, 1, p.n);
    json inverse = {{"op", "add"}, {"path", path}, {"value", std::move(*it)}};
    parent->erase(it);
    res_.modified.push_back(t.id);
    note_timing(t.id, p);
    done({{"op", "remove"}, {"path", path}}, std::move(inverse));
    return {};
  }

  Result<void> op_move(const json &op, const Path &p) {
    ATM_TRY(Target t, resolve(p.last()));
    const auto to_it = op.find("to");
    if (to_it == op.end() || !to_it->is_string())
      return fail(ErrorCode::InvalidArgument, "P_PATH", "\"move\" needs \"to\": \"<parentId>/<collection>\".");
    ATM_TRY(Path tp, split(to_it->get_ref<const std::string &>()));
    if (tp.n < 2)
      return fail(ErrorCode::InvalidArgument, "P_PATH", "\"to\" must be \"<parentId>/<collection>\".");
    ATM_TRY(Target np, resolve(tp.seg[0]));
    const string_view coll_name = tp.last();
    if (const string_view prefix = doc::prefix_for_collection(coll_name); !prefix.empty() && prefix != id_prefix(t.id))
      return fail(ErrorCode::SchemaViolation, "S_ID_PREFIX",
                  "\"" + t.id + "\" cannot live in the collection \"" + std::string(coll_name) + "\".");
    for (std::string cur = np.id; !cur.empty(); cur = doc_.find(cur)->parent)
      if (cur == t.id)
        return fail(ErrorCode::CycleDetected, "P_CYCLE", "An object cannot be moved into itself.");
    ATM_TRY(Home h, home_of(t.id));

    json *container = ensure(np.node, tp, 1, tp.n - 1, np.id);
    if (!container || !container->is_object())
      return fail(ErrorCode::InvalidArgument, "P_PATH", "\"to\" does not lead to a collection.");
    const std::string container_path = tp.n > 2 ? np.id + "/" + join(tp, 1, tp.n - 1) : np.id;
    json &map = ensure_key(*container, std::string(coll_name), json::object(), container_path);
    if (!map.is_object())
      return fail(ErrorCode::InvalidArgument, "P_PATH", "\"to\" does not lead to a collection.");
    const bool same = &map == h.map;
    const std::string new_coll = join(tp, 1, tp.n);
    const std::string order_key = doc::order_key_for(coll_name);

    size_t pos = kNone;
    if (!order_key.empty()) {
      const auto order = container->find(order_key);
      if (same && h.order_index != kNone) { // anchors are resolved against the order without the moved object
        json rest = *h.order;
        rest.erase(h.order_index);
        ATM_TRY(size_t q, anchor_pos(&rest, op));
        pos = q;
      } else {
        ATM_TRY(size_t q, anchor_pos(order == container->end() ? nullptr : &*order, op));
        pos = q;
      }
    }
    json inverse = {{"op", "move"}, {"path", t.id}, {"to", h.parent + "/" + h.collection}};
    if (json a = old_anchor(h); !a.is_null())
      inverse["anchor"] = std::move(a);

    if (h.order_index != kNone)
      h.order->erase(h.order_index);
    if (!same) {
      const auto it = h.map->find(t.id);
      doc_.unindex_subtree(*it, t.id);
      json value = std::move(*it);
      h.map->erase(it);
      const auto moved = map.emplace(t.id, std::move(value)).first;
      doc_.index_subtree(*moved, t.id, np.id, new_coll);
    }
    if (pos != kNone)
      insert_at(*container, order_key, pos, t.id, container_path);

    json forward = {{"op", "move"}, {"path", t.id}, {"to", np.id + "/" + new_coll}};
    if (json a = anchor_json(op); !a.is_null())
      forward["anchor"] = std::move(a);
    if (id_prefix(t.id) == "clp")
      timing_clips_.insert(t.id);
    res_.modified.push_back(t.id);
    done(std::move(forward), std::move(inverse));
    return {};
  }

  Result<void> insert_order(const json &op, const Path &p) {
    ATM_TRY(Target t, resolve(p.seg[0]));
    const auto vit = op.find("value");
    json *container = p.n >= 2 ? descend(t.node, p, 1, p.n - 1) : nullptr;
    if (!container || !container->is_object() || !ends_with_order(p.last()) || vit == op.end() || !vit->is_string())
      return fail(ErrorCode::InvalidArgument, "P_PATH",
                  "\"insert_order\" needs a path \"<parentId>/<name>_order\" and an ID \"value\".");
    const std::string id = resolve_ref(vit->get_ref<const std::string &>());
    const std::string key(p.last());
    const auto arr = container->find(key);
    const json *existing = arr == container->end() ? nullptr : &*arr;
    if (existing && existing->is_array() && index_of(*existing, id) != kNone)
      return fail(ErrorCode::InvalidArgument, "P_EXISTS", "\"" + id + "\" is already in this order.", {},
                  "Use \"move\" to change its position.");
    ATM_TRY(size_t pos, anchor_pos(existing, op));
    insert_at(*container, key, pos, id, p.n > 2 ? t.id + "/" + join(p, 1, p.n - 1) : t.id);
    const std::string path = t.id + "/" + join(p, 1, p.n);
    json forward = {{"op", "insert_order"}, {"path", path}, {"value", id}};
    if (json a = anchor_json(op); !a.is_null())
      forward["anchor"] = std::move(a);
    res_.modified.push_back(t.id);
    done(std::move(forward), {{"op", "remove_order"}, {"path", path}, {"value", id}});
    return {};
  }

  Result<void> remove_order(const json &op, const Path &p) {
    ATM_TRY(Target t, resolve(p.seg[0]));
    const auto vit = op.find("value");
    json *arr = p.n >= 2 ? descend(t.node, p, 1, p.n) : nullptr;
    if (!arr || !arr->is_array() || !ends_with_order(p.last()) || vit == op.end() || !vit->is_string())
      return fail(ErrorCode::InvalidArgument, "P_PATH",
                  "\"remove_order\" needs a path \"<parentId>/<name>_order\" and an ID \"value\".");
    const std::string id = resolve_ref(vit->get_ref<const std::string &>());
    const size_t idx = index_of(*arr, id);
    if (idx == kNone)
      return fail(ErrorCode::UnknownId, "P_NOT_IN_ORDER", "\"" + id + "\" is not in this order.");
    const std::string path = t.id + "/" + join(p, 1, p.n);
    json inverse = {{"op", "insert_order"}, {"path", path}, {"value", id}, {"anchor", anchor_at(*arr, idx)}};
    arr->erase(idx);
    res_.modified.push_back(t.id);
    done({{"op", "remove_order"}, {"path", path}, {"value", id}}, std::move(inverse));
    return {};
  }

  Result<void> op_test(const json &op, const Path &p) {
    ATM_TRY(Target t, resolve(p.seg[0]));
    json expected = op.contains("value") ? op["value"] : json(nullptr);
    replace_refs(expected);
    if (p.n >= 2)
      ATM_CHECK(normalize_field(expected, p.last(), t.id));
    const json *cur = descend(t.node, p, 1, p.n);
    const json got = cur ? *cur : json(nullptr);
    if (got != expected) {
      Error e;
      e.code = ErrorCode::ConflictingPatch;
      e.rule = "P_TEST_FAILED";
      e.message = "A \"test\" op did not match; the project changed since the patch was made.";
      e.hint = "Read the current value with: attome get <project> " + t.id + ", then rebuild the patch.";
      e.details = {{"expected", expected}, {"got", got}};
      return tl::unexpected(std::move(e));
    }
    const std::string path = p.n >= 2 ? t.id + "/" + join(p, 1, p.n) : t.id;
    done({{"op", "test"}, {"path", path}, {"value", std::move(expected)}}, nullptr);
    return {};
  }

  doc::Document &doc_;
  bool internal_ = false;
  std::vector<json> prunes_;
  ApplyResult res_;
  std::vector<json> inv_;
  std::set<std::string> timing_clips_;
};

} // namespace

Result<ApplyResult> apply(doc::Document &doc, const json &ops, const ApplyOptions &options) {
  ATM_PROFILE_SCOPE("patch.apply");
  Applier applier(doc, !options.validate);
  Result<void> r;
  {
    ATM_PROFILE_SCOPE("patch.ops");
    r = applier.run(ops);
  }
  if (r && options.validate) {
    ATM_PROFILE_SCOPE("patch.validate");
    r = applier.validate();
  }
  if (!r || !options.keep) {
    ATM_PROFILE_SCOPE("patch.rollback");
    Applier undo(doc, true);
    if (auto back = undo.run(applier.inverse()); !back)
      return fail(ErrorCode::Internal, "P_ROLLBACK", "A patch could not be rolled back: " + back.error().message,
                  {}, "Close the project without saving and open it again.");
  }
  if (!r)
    return tl::unexpected(std::move(r.error()));
  return applier.take();
}

json validate_document(const doc::Document &doc) {
  ATM_PROFILE_SCOPE("doc.validate");
  json problems = json::array();
  const json &root = doc.root();
  auto check_order = [&](const json &owner, const std::string &owner_id, const char *map_key, const char *order_key) {
    const auto map = owner.find(map_key);
    const auto order = owner.find(order_key);
    const size_t in_map = map != owner.end() && map->is_object() ? map->size() : 0;
    const size_t in_order = order != owner.end() && order->is_array() ? order->size() : 0;
    bool ok = in_map == in_order;
    for (size_t i = 0; ok && i < in_order; ++i)
      ok = (*order)[i].is_string() && map->contains((*order)[i].get_ref<const std::string &>());
    if (!ok && problems.size() < kMaxProblems)
      problems.push_back(problem("R_ORDER_MISMATCH", owner_id + "/" + order_key, owner_id,
                                 std::string("\"") + order_key + "\" does not list exactly the objects in \"" +
                                     map_key + "\".",
                                 "Fix it with insert_order / remove_order ops."));
  };
  check_order(root, doc.id(), "sequences", "sequence_order");
  const auto seqs = root.find("sequences");
  if (seqs == root.end() || !seqs->is_object())
    return problems;
  for (auto s = seqs->begin(); s != seqs->end(); ++s) {
    check_order(*s, s.key(), "tracks", "track_order");
    const auto tracks = s->find("tracks");
    if (tracks == s->end() || !tracks->is_object())
      continue;
    for (auto t = tracks->begin(); t != tracks->end(); ++t) {
      check_order(*t, t.key(), "clips", "clip_order");
      check_track(doc, t.key(), problems);
    }
  }
  return problems;
}

} // namespace atm::patch
