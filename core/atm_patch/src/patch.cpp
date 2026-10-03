#include "atm/patch/patch.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <map>
#include <optional>
#include <set>

#include "atm/base/id.hpp"
#include "atm/base/profiler.hpp"
#include "atm/base/time.hpp"
#include "atm/eval/effects.hpp"
#include "atm/eval/keyframes.hpp"

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
  return k == "record_in" || k == "duration" || k == "source_in" || k == "t" || k == "start" || k == "in_offset" ||
         k == "out_offset" || k == "fade_in" || k == "fade_out";
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

struct Span {
  Rational in, end;
  const std::string *id;
};

// Media on either side of a clip's used range, or nullopt when unlimited (text, still pictures) or unknown (no
// media_ref.duration).
struct Handles {
  std::optional<Rational> before, after;
};

Handles handles_of(const json &clip, const Rational &duration) {
  Handles h;
  const json ref = clip.value("media_ref", json::object());
  if (const std::string type = ref.value("type", ""); type == "text" || type == "image")
    return h;
  const json timing = clip.value("timing", json::object());
  const auto source_in = Rational::parse(timing.value("source_in", std::string("0")));
  if (!source_in)
    return h;
  h.before = *source_in;
  if (const auto media = ref.find("duration"); media != ref.end() && media->is_string())
    if (const auto total = Rational::parse(media->get_ref<const std::string &>()))
      if (const auto used = add(*source_in, duration))
        if (const auto rest = sub(*total, *used))
          h.after = *rest;
  return h;
}

// Transition rules (F1 §4.1): a dissolve joins two clips that touch on this track, and each clip has enough media
// beyond the cut for the overlap: `from` plays out_offset past its out point, `to` starts in_offset before its in point.
void check_transitions(const json &track, const std::string &track_id, const std::vector<Span> &spans, json &problems) {
  const auto transitions = track.find("transitions");
  if (transitions == track.end() || !transitions->is_object())
    return;
  const json &clips = track["clips"];
  std::map<std::string, Rational> used; // per clip: the part of its duration that transitions already overlap
  for (auto it = transitions->begin(); it != transitions->end() && problems.size() < kMaxProblems; ++it) {
    const std::string &id = it.key();
    const json &t = *it;
    const std::string kind = eval::transition_id(t.value("type", ""));
    if (kind.empty()) {
      problems.push_back(problem("TRANSITION_UNSUPPORTED", id + "/type", id,
                                 "Transition " + id + " has the type \"" + t.value("type", "") + "\".",
                                 "Use \"type\": \"attome.dissolve\", \"attome.wipe\" or \"attome.push\" (" + eval::transition_ids() + ")."));
      continue;
    }
    if (kind == "wipe" || kind == "push") {
      const json params = t.value("params", json::object());
      eval::WipeDirection dir;
      const auto d = params.is_object() ? params.find("direction") : params.end();
      const auto s = params.is_object() ? params.find("softness") : params.end();
      if (d != params.end() && !(d->is_string() && eval::parse_wipe_direction(d->get<std::string>(), dir))) {
        problems.push_back(problem("TRANSITION_PARAM", id + "/params/direction", id,
                                   "The " + kind + " " + id + " has an unknown params.direction.",
                                   "Use \"left\", \"right\", \"up\" or \"down\": the side the incoming clip enters from."));
        continue;
      }
      if (kind == "wipe" && s != params.end() && (!s->is_number() || s->get<double>() < 0.01 || s->get<double>() > 1.0)) {
        problems.push_back(problem("TRANSITION_PARAM", id + "/params/softness", id,
                                   "The wipe " + id + " needs params.softness from 0.01 to 1.",
                                   "softness is the width of the soft edge as a fraction of the picture; 0.1 is a good start."));
        continue;
      }
    }
    const std::string from = t.value("from", ""), to = t.value("to", "");
    const auto find = [&](const std::string &clip) -> const Span * {
      for (const Span &s : spans)
        if (*s.id == clip)
          return &s;
      return nullptr;
    };
    const Span *a = find(from), *b = find(to);
    if (!a || !b || compare(a->end, b->in) != 0) {
      problems.push_back(problem(
          "TRANSITION_NOT_ADJACENT", id, id,
          "Transition " + id + " needs \"from\" and \"to\" to be clips on track " + track_id +
              " where \"from\" ends exactly where \"to\" starts.",
          !a || !b ? "Remove the transition (op remove, path " + id + ") or point it at clips of this track."
                   : "Set " + to + "/timing/record_in to \"" + a->end.to_string() + "\", or remove the transition."));
      continue;
    }
    const auto in_off = Rational::parse(t.value("in_offset", std::string("0")));
    const auto out_off = Rational::parse(t.value("out_offset", std::string("0")));
    if (!in_off || !out_off || in_off->num() < 0 || out_off->num() < 0 || (in_off->num() == 0 && out_off->num() == 0)) {
      problems.push_back(problem("TRANSITION_DURATION", id, id,
                                 "Transition " + id + " needs in_offset and out_offset of zero or more, not both zero.",
                                 "For a 1-second dissolve centred on the cut use in_offset \"0.5s\", out_offset \"0.5s\"."));
      continue;
    }
    // The dissolve covers [cut - in_offset, cut + out_offset): `from` must still be on screen at its start, `to` at
    // its end, and a clip with transitions at both ends must be long enough for both.
    const Rational a_dur = *sub(a->end, a->in), b_dur = *sub(b->end, b->in);
    const Rational a_used = used.count(from) ? used.at(from) : Rational(), b_used = used.count(to) ? used.at(to) : Rational();
    const Rational a_room = *sub(a_dur, a_used), b_room = *sub(b_dur, b_used);
    if (compare(*in_off, a_room) > 0 || compare(*out_off, b_room) > 0) {
      problems.push_back(problem("TRANSITION_TOO_LONG", id, id,
                                 "Transition " + id + " is longer than the clips it joins.",
                                 "Keep in_offset at most " + seconds_text(a_room) + " s and out_offset at most " +
                                     seconds_text(b_room) + " s."));
      continue;
    }
    used[from] = *add(a_used, *in_off);
    used[to] = *add(b_used, *out_off);
    const Handles ha = handles_of(clips[from], a_dur), hb = handles_of(clips[to], b_dur);
    const bool out_short = ha.after && compare(*out_off, *ha.after) > 0;
    const bool in_short = hb.before && compare(*in_off, *hb.before) > 0;
    if (out_short || in_short) {
      json p = problem("TRANSITION_INSUFFICIENT_HANDLES", id + (out_short ? "/out_offset" : "/in_offset"), id,
                          out_short ? "Clip " + from + " has only " + seconds_text(*ha.after) +
                                          " s of media after its out point, less than out_offset " +
                                          seconds_text(*out_off) + " s."
                                    : "Clip " + to + " has only " + seconds_text(*hb.before) +
                                          " s of media before its in point, less than in_offset " +
                                          seconds_text(*in_off) + " s.",
                          [&] {
                            // The longest dissolve centred on the cut: twice the smaller of the two spares.
                            std::string hint = "Use in_offset at most " +
                                               (hb.before ? seconds_text(*hb.before) : std::string("any")) +
                                               " s and out_offset at most " +
                                               (ha.after ? seconds_text(*ha.after) : std::string("any")) + " s";
                            if (ha.after && hb.before) {
                              const Rational spare = compare(*ha.after, *hb.before) < 0 ? *ha.after : *hb.before;
                              hint += " (a centred dissolve of at most " + seconds_text(add(spare, spare).value_or(spare)) + " s)";
                            }
                            return hint + ", or move the clips' source_in to leave more media beyond the cut.";
                          }());
      p["details"] = {{"max_in_offset", hb.before ? json(hb.before->to_string()) : json(nullptr)},
                      {"max_out_offset", ha.after ? json(ha.after->to_string()) : json(nullptr)}};
      problems.push_back(std::move(p));
    }
  }
}

// Transform values (F1 §3.1 item 4): opacity and rotation (numbers), position and anchor ([x, y]), scale (a number or
// [x, y]) and crop ({left, top, right, bottom}, fractions of the picture). All but crop can be animated (F1 §5.4).
void check_keyframes(const json &clip, const std::string &clip_id, json &problems) {
  const auto tr = clip.find("transform");
  if (tr == clip.end() || !tr->is_object())
    return;
  // Static values first: an animation written in their place would otherwise be stored and quietly ignored.
  const auto is_pair = [](const json &v) { return v.is_array() && v.size() == 2 && v[0].is_number() && v[1].is_number(); };
  for (const char *key : {"opacity", "rotation", "position", "anchor", "scale"})
    if (const auto v = tr->find(key); v != tr->end()) {
      const std::string k = key;
      const bool number = k == "opacity" || k == "rotation", pair = k == "position" || k == "anchor";
      const bool ok = number ? v->is_number() : pair ? is_pair(*v) : (v->is_number() || is_pair(*v));
      if (!ok)
        problems.push_back(problem("TRANSFORM_TYPE_MISMATCH", clip_id + "/transform/" + k, clip_id,
                                   "transform." + k + " of clip " + clip_id + " must be " +
                                       (number ? "a number" : pair ? "[x, y]" : "a number or [x, y]") + ".",
                                   "To animate it, put keys in transform.keyframes." + k +
                                       " (guide.get topic \"keyframes\") and leave a plain value here."));
    }
  if (const auto crop = tr->find("crop"); crop != tr->end()) {
    const std::string path = clip_id + "/transform/crop";
    const auto bad = [&](const std::string &what) {
      problems.push_back(problem("TRANSFORM_CROP", path, clip_id, "transform.crop of clip " + clip_id + " " + what,
                                 "Write {\"left\": 0.1, \"right\": 0.1}: each side a fraction of the picture from 0 to 1, "
                                 "left + right and top + bottom below 1."));
    };
    if (!crop->is_object()) {
      bad("must be an object.");
    } else {
      bool sides_ok = true;
      for (auto it = crop->begin(); it != crop->end() && sides_ok; ++it) {
        const std::string &side = it.key();
        if (side != "left" && side != "top" && side != "right" && side != "bottom") {
          bad("has \"" + side + "\"; the sides are left, top, right and bottom.");
          sides_ok = false;
        } else if (!it->is_number() || it->get<double>() < 0.0 || it->get<double>() >= 1.0) {
          bad("has " + side + " " + it->dump() + "; it must be a number from 0 to below 1.");
          sides_ok = false;
        }
      }
      const auto side = [&](const char *key) { return crop->value(key, 0.0); };
      if (sides_ok && side("left") + side("right") >= 1.0)
        bad("cuts away the whole width: left + right must be below 1.");
      else if (sides_ok && side("top") + side("bottom") >= 1.0)
        bad("cuts away the whole height: top + bottom must be below 1.");
    }
  }
  const auto kfs = tr->find("keyframes");
  if (kfs == tr->end())
    return;
  if (!kfs->is_object()) {
    problems.push_back(problem("KEYFRAME_TYPE_MISMATCH", clip_id + "/transform/keyframes", clip_id,
                               "transform.keyframes of clip " + clip_id + " must be a map of properties.",
                               "Write {\"opacity\": {\"$new:k1\": {\"t\": \"0s\", \"v\": 0}, …}}."));
    return;
  }
  for (auto it = kfs->begin(); it != kfs->end() && problems.size() < kMaxProblems; ++it) {
    const std::string &prop = it.key();
    const std::string path = clip_id + "/transform/keyframes/" + prop;
    const int dims = (prop == "opacity" || prop == "rotation") ? 1
                     : (prop == "position" || prop == "scale" || prop == "anchor") ? 2
                                                                                    : 0;
    if (dims == 0) {
      problems.push_back(problem("KEYFRAME_PROPERTY_UNSUPPORTED", path, clip_id,
                                 "The property \"" + prop + "\" of clip " + clip_id + " cannot be animated yet.",
                                 "Animate opacity, position, scale, rotation or anchor (crop stays fixed for now)."));
      continue;
    }
    if (auto curve = eval::parse_curve(*it, dims); !curve)
      problems.push_back(problem(curve.error().rule, curve.error().path.empty() ? path : path + "/" + curve.error().path,
                                 clip_id, curve.error().message, curve.error().hint));
  }
}

// Sound settings (F1 §4.1): clip "audio" {gain_db, pan, fade_in, fade_out, fade_curve}; track volume_db and pan.
void check_number(const json &obj, const char *key, double lo, double hi, const std::string &owner, const std::string &path,
                  json &problems) {
  const auto v = obj.find(key);
  if (v == obj.end() || (v->is_number() && v->get<double>() >= lo && v->get<double>() <= hi))
    return;
  char range[64];
  std::snprintf(range, sizeof range, "a number from %g to %g", lo, hi);
  problems.push_back(problem("AUDIO_TYPE_MISMATCH", path + "/" + key, owner,
                             std::string(key) + " of " + owner + " must be " + range + ".",
                             std::string(key) == "pan" ? "-1 is full left, 0 the centre, 1 full right."
                                                       : "Gain in decibels: -12 is about a quarter of the level, 0 unchanged."));
}

void check_audio(const json &clip, const std::string &clip_id, const Rational &duration, json &problems) {
  const auto it = clip.find("audio");
  if (it == clip.end())
    return;
  const std::string path = clip_id + "/audio";
  if (!it->is_object()) {
    problems.push_back(problem("AUDIO_TYPE_MISMATCH", path, clip_id, "audio of clip " + clip_id + " must be an object.",
                               "Write {\"gain_db\": -12, \"fade_out\": \"2s\"}."));
    return;
  }
  check_number(*it, "gain_db", -96.0, 24.0, clip_id, path, problems);
  check_number(*it, "pan", -1.0, 1.0, clip_id, path, problems);
  const std::string curve = it->value("fade_curve", std::string("equal_power"));
  if (curve != "equal_power" && curve != "linear")
    problems.push_back(problem("AUDIO_TYPE_MISMATCH", path + "/fade_curve", clip_id,
                               "fade_curve of clip " + clip_id + " is \"" + curve + "\".",
                               "Use \"equal_power\" (the default) or \"linear\"."));
  Rational total;
  for (const char *key : {"fade_in", "fade_out"}) {
    const auto f = it->find(key);
    if (f == it->end())
      continue;
    std::optional<Rational> t;
    if (f->is_string())
      if (auto r = Rational::parse(f->get_ref<const std::string &>()))
        t = *r;
    if (!t || t->num() < 0) {
      problems.push_back(problem("AUDIO_TYPE_MISMATCH", path + "/" + key, clip_id,
                                 std::string(key) + " of clip " + clip_id + " must be a time of zero or more.",
                                 "Write a time such as \"2s\"."));
      return;
    }
    total = add(total, *t).value_or(total);
  }
  if (compare(total, duration) > 0)
    problems.push_back(problem("AUDIO_FADE_TOO_LONG", path, clip_id,
                               "The fades of clip " + clip_id + " (" + seconds_text(total) + " s together) are longer than the clip (" +
                                   seconds_text(duration) + " s).",
                               "Keep fade_in + fade_out at most the clip's duration."));
}

// Effects (ADR-004): on picture clips (the clip alone) and on adjustment layers (everything below). Only the Gaussian
// blur so far.
void check_effects(const json &clip, const std::string &clip_id, bool audio_track, json &problems) {
  const auto fx = clip.find("effects");
  if (fx == clip.end() || (fx->is_object() && fx->empty()))
    return;
  if (audio_track || !fx->is_object()) {
    problems.push_back(problem("EFFECT_UNSUPPORTED", clip_id + "/effects", clip_id,
                               audio_track ? "Clip " + clip_id + " is on an audio track; picture effects do not apply to sound."
                                           : "effects of clip " + clip_id + " must be a map of effect objects.",
                               "Put picture effects on clips of video tracks (guide.get topic \"effects\")."));
    return;
  }
  for (auto it = fx->begin(); it != fx->end() && problems.size() < kMaxProblems; ++it) {
    const std::string &id = it.key();
    const std::string name = it->is_object() ? it->value("effect", std::string()) : std::string();
    const eval::EffectDef *def = eval::find_effect(name);
    if (!def) {
      problems.push_back(problem("EFFECT_UNSUPPORTED", id + "/effect", clip_id,
                                 "Effect " + id + " is \"" + name + "\", which is not available.",
                                 "Use \"attome.<id>@1.0.0\" with one of: " + eval::effect_ids() + "."));
      continue;
    }
    const json params = it->value("params", json::object());
    if (!params.is_object()) {
      problems.push_back(problem("EFFECT_PARAM", id + "/params", clip_id, "params of effect " + id + " must be an object.",
                                 "Give the parameters by name, for example {\"" + std::string(def->params[0].key) + "\": " +
                                     std::to_string(def->params[0].def) + "}."));
      continue;
    }
    // Keyframes of the parameters ("keyframes": {"radius": {"kf_1": {t, v, interp, ease}, ...}}): a parameter with keys
    // needs no plain value, and every key's value stays inside the parameter's range.
    const auto kfs = it->find("keyframes");
    if (kfs != it->end() && !kfs->is_object()) {
      problems.push_back(problem("KEYFRAME_TYPE_MISMATCH", id + "/keyframes", clip_id,
                                 "keyframes of effect " + id + " must be a map of parameters.",
                                 "Write {\"" + std::string(def->params[0].key) + "\": {\"$new:k1\": {\"t\": \"0s\", \"v\": " +
                                     std::to_string(def->params[0].def) + "}, ...}}."));
      continue;
    }
    if (kfs != it->end())
      for (auto k = kfs->begin(); k != kfs->end() && problems.size() < kMaxProblems; ++k) {
        const eval::EffectParam *param = nullptr;
        for (const eval::EffectParam &p : def->params)
          if (k.key() == p.key)
            param = &p;
        const std::string path = id + "/keyframes/" + k.key();
        if (!param) {
          problems.push_back(problem("KEYFRAME_PROPERTY_UNSUPPORTED", path, clip_id,
                                     "The effect " + id + " has no parameter \"" + k.key() + "\" to animate.",
                                     "Its parameters are: " + [&] {
                                       std::string names;
                                       for (const eval::EffectParam &p : def->params)
                                         names += (names.empty() ? "" : ", ") + std::string(p.key);
                                       return names;
                                     }() + "."));
          continue;
        }
        auto curve = eval::parse_curve(*k, 1);
        if (!curve) {
          problems.push_back(problem(curve.error().rule, curve.error().path.empty() ? path : path + "/" + curve.error().path,
                                     clip_id, curve.error().message, curve.error().hint));
          continue;
        }
        for (const eval::Key &key : curve->keys)
          if (key.v[0] < param->lo || key.v[0] > param->hi) {
            problems.push_back(problem("EFFECT_PARAM", path, clip_id,
                                       "A keyframe of " + std::string(param->title) + " in effect " + id + " has the value " +
                                           std::to_string(key.v[0]) + "; it must be from " + std::to_string(param->lo) + " to " +
                                           std::to_string(param->hi) + ".",
                                       "Keep every key inside the parameter's range."));
            break;
          }
      }
    for (const eval::EffectParam &p : def->params) {
      const auto v = params.find(p.key);
      const bool missing = v == params.end();
      const bool animated = kfs != it->end() && kfs->is_object() && kfs->contains(p.key) && !(*kfs)[p.key].empty();
      if ((missing && (!p.required || animated)) || problems.size() >= kMaxProblems)
        continue;
      if (missing || !v->is_number() || v->get<double>() < p.lo || v->get<double>() > p.hi)
        problems.push_back(problem("EFFECT_PARAM", id + "/params/" + p.key, clip_id,
                                   "The " + std::string(def->title) + " effect " + id + " needs params." + p.key + " from " +
                                       std::to_string(p.lo) + " to " + std::to_string(p.hi) + ".",
                                   std::string(p.title) + " defaults to " + std::to_string(p.def) +
                                       "; a blur radius is a fraction of the picture height (0.02 soft, 0.1 strong)."));
    }
  }
}

// Timing rules of one track: every clip has a valid timing, clips do not overlap, and transitions fit.
void check_track(const doc::Document &doc, const std::string &track_id, json &problems) {
  const NodeRef *ref = doc.find(track_id);
  if (!ref)
    return;
  check_number(*ref->node, "volume_db", -96.0, 24.0, track_id, track_id, problems);
  check_number(*ref->node, "pan", -1.0, 1.0, track_id, track_id, problems);
  const auto clips = ref->node->find("clips");
  if (clips == ref->node->end() || !clips->is_object())
    return;
  std::vector<Span> spans;
  spans.reserve(clips->size());
  for (auto it = clips->begin(); it != clips->end() && problems.size() < kMaxProblems; ++it) {
    const std::string &id = it.key();
    check_keyframes(*it, id, problems);
    if (const auto mref = it->find("media_ref"); mref != it->end() && mref->is_object() && mref->contains("stream")) {
      const std::string stream = mref->value("stream", std::string());
      if (stream != "video" && stream != "audio")
        problems.push_back(problem("MEDIA_STREAM", id + "/media_ref/stream", id,
                                   "media_ref.stream of clip " + id + " is \"" + stream + "\".",
                                   "Use \"video\" (picture only) or \"audio\" (sound only), or leave it out for both."));
    }
    check_effects(*it, id, ref->node->value("kind", "video") == "audio", problems);
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
    check_audio(*it, id, *rdur, problems);
    spans.push_back({*rin, *end, &id});
  }
  std::sort(spans.begin(), spans.end(), [](const Span &a, const Span &b) { return compare(a.in, b.in) < 0; });
  for (size_t i = 1; i < spans.size() && problems.size() < kMaxProblems; ++i) {
    const Span &a = spans[i - 1], &b = spans[i];
    if (compare(a.end, b.in) <= 0)
      continue;
    problems.push_back(problem(
        "R_TRACK_OVERLAP", *b.id + "/timing", track_id,
        "Clip " + *b.id + " (" + seconds_text(b.in) + " to " + seconds_text(b.end) + " s) overlaps clip " + *a.id + " (" +
            seconds_text(a.in) + " to " + seconds_text(a.end) + " s) on track " + track_id + ".",
        "Set " + *b.id + "/timing/record_in to \"" + a.end.to_string() + "\", or shorten " + *a.id + " first."));
  }
  check_transitions(*ref->node, track_id, spans, problems);
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
    for (const std::string &id : left_tracks_)
      if (doc_.find(id))
        tracks.insert(id);
    for (const std::string &id : timing_clips_) // clips, transitions and keyframes: check the track that holds them
      if (id_prefix(id) == "trk" && doc_.find(id))
        tracks.insert(id);
      else
        for (const NodeRef *ref = doc_.find(id); ref && !ref->parent.empty(); ref = doc_.find(ref->parent))
          if (id_prefix(ref->parent) == "trk") {
            tracks.insert(ref->parent);
            break;
          }
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
    const string_view prefix = id_prefix(id);
    if ((prefix == "clp" && p.n >= 2 &&
         (p.seg[1] == "timing" || p.seg[1] == "media_ref" || p.seg[1] == "transform" || p.seg[1] == "audio" ||
          p.seg[1] == "effects")) ||
        prefix == "trn" || prefix == "kf" || prefix == "trk" || prefix == "fx")
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
    if (prefix == "clp" || prefix == "trn" || prefix == "kf" || prefix == "fx")
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
    if (id_prefix(h.parent) == "trk") // a removed clip may leave a transition without its clip
      left_tracks_.insert(h.parent);
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
    if (id_prefix(h.parent) == "trk")
      left_tracks_.insert(h.parent);

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
  std::set<std::string> left_tracks_; // tracks an object was removed or moved from
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
