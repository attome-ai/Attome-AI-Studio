#include "timeline.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <vector>

#include "atm/base/id.hpp"
#include "atm/base/time.hpp"
#include "atm/eval/effects.hpp"

namespace atm::api::timeline {
namespace {

using doc::Document;

// An effect object for the document from `src`: each parameter of the definition by name, its default when missing.
json effect_object(const eval::EffectDef &def, const json &src) {
  json params = json::object();
  for (const eval::EffectParam &p : def.params) {
    const auto v = src.is_object() ? src.find(p.key) : src.end();
    params[p.key] = v != src.end() && v->is_number() ? v->get<double>() : p.def;
  }
  return {{"effect", eval::effect_name(def)}, {"enabled", true}, {"params", std::move(params)}};
}

// A transition object ("dissolve", "wipe" or "push") between two clips; a wipe and a push carry their params.
json transition_value(const std::string &kind, const std::string &from, const std::string &to, const Rational &in,
                      const Rational &out, const json &params) {
  json v = {{"type", "attome." + kind}, {"from", from}, {"to", to}, {"in_offset", in.to_string()}, {"out_offset", out.to_string()}};
  if (!params.empty())
    v["params"] = params;
  return v;
}

Rational plus(Rational a, Rational b) { return add(a, b).value_or(a); }
Rational minus(Rational a, Rational b) { return sub(a, b).value_or(a); }
Rational at_most_zero(Rational a) { return a.num() < 0 ? Rational() : a; }

// Clip timing in rationals.
struct Span {
  Rational in, duration, source_in;
  Rational end() const { return plus(in, duration); }
};

Span span_of(const json &clip) {
  const json timing = clip.value("timing", json::object());
  const auto field = [&](const char *key) {
    return Rational::parse(timing.value(key, std::string("0"))).value_or(Rational());
  };
  return {field("record_in"), field("duration"), field("source_in")};
}

class Builder {
public:
  Builder(const Document &doc, const json &op, size_t index, const Context &ctx)
      : doc_(doc), op_(op), index_(index), ctx_(ctx) {}

  Result<Built> run() {
    const std::string name = op_.value("op", std::string());
    if (name == "add_track")
      return wrap(add_track());
    if (name == "add_clip")
      return wrap(add_clip());
    if (name == "add_text")
      return wrap(add_text());
    if (name == "add_adjustment")
      return wrap(add_adjustment());
    if (name == "add_transition")
      return wrap(add_transition());
    if (name == "delete" || name == "ripple_delete")
      return wrap(remove(name == "ripple_delete"));
    if (name == "move")
      return wrap(move());
    if (name == "trim")
      return wrap(trim());
    if (name == "split")
      return wrap(split());
    if (name == "set_property")
      return wrap(set_property());
    if (name == "add_effect")
      return wrap(add_effect());
    if (name == "link")
      return wrap(link());
    if (name == "unlink")
      return wrap(unlink());
    if (name == "remove_effect" || name == "set_effect_enabled")
      return wrap(change_effect(name == "remove_effect"));
    if (name == "slip")
      return wrap(slip());
    if (name == "roll")
      return wrap(roll());
    if (name == "slide")
      return wrap(slide());
    return fail("E_OP", "\"" + name + "\" is not a timeline op.",
                "Use add_track, add_clip, add_text, add_adjustment, add_transition, delete, ripple_delete, move, trim, "
                "split, slip, roll, slide, add_effect, remove_effect, set_effect_enabled, link, unlink or "
                "set_property (guide.get topic \"timeline\").");
  }

private:
  const Document &doc_;
  const json &op_;
  size_t index_;
  const Context &ctx_;
  Built out_;

  Result<Built> wrap(Result<void> r) {
    if (!r)
      return tl::unexpected(std::move(r.error()));
    return std::move(out_);
  }

  tl::unexpected<Error> fail(std::string rule, std::string message, std::string hint = {}) const {
    Error e;
    e.code = ErrorCode::InvalidArgument;
    e.rule = std::move(rule);
    e.message = "ops[" + std::to_string(index_) + "] (" + op_.value("op", std::string("?")) + "): " + message;
    e.hint = std::move(hint);
    e.details = {{"op_index", index_}};
    return tl::unexpected(std::move(e));
  }

  // ---- reading the document ------------------------------------------------------------------------------------

  const json &seq() const { return doc_.root()["sequences"][ctx_.sequence]; }

  std::vector<std::string> track_order() const {
    std::vector<std::string> out;
    if (const auto o = seq().find("track_order"); o != seq().end() && o->is_array())
      for (const json &id : *o)
        if (id.is_string())
          out.push_back(id.get<std::string>());
    return out;
  }

  const json *track(const std::string &id) const {
    const auto tracks = seq().find("tracks");
    if (tracks == seq().end() || !tracks->contains(id))
      return nullptr;
    return &(*tracks)[id];
  }

  const json *track_named(const std::string &name) const {
    for (const std::string &id : track_order())
      if (const json *t = track(id); t && t->value("name", "") == name)
        return t;
    return nullptr;
  }

  std::string track_id_named(const std::string &name) const {
    for (const std::string &id : track_order())
      if (const json *t = track(id); t && t->value("name", "") == name)
        return id;
    return {};
  }

  // A clip of this sequence and the track that holds it.
  Result<std::pair<const json *, std::string>> clip(const char *key) const {
    const std::string id = op_.value(key, std::string());
    const doc::NodeRef *ref = id.empty() ? nullptr : doc_.find(id);
    if (!ref || id_prefix(id) != "clp" || !track(ref->parent))
      return fail("E_UNKNOWN_CLIP", "\"" + std::string(key) + "\" must be the ID of a clip of this sequence, not \"" + id + "\".",
                  "Read the clip IDs with project.inspect level \"tracks\", or use the $new: name of a clip made earlier in this call.");
    return std::pair<const json *, std::string>{ref->node, ref->parent};
  }

  Rational track_end(const std::string &track_id) const {
    Rational end;
    if (const json *t = track(track_id); t && t->contains("clips"))
      for (const json &c : (*t)["clips"])
        if (compare(span_of(c).end(), end) > 0)
          end = span_of(c).end();
    return end;
  }

  // An optional time field; missing gives nullopt, a bad value an error.
  Result<std::optional<Rational>> time(const char *key) const {
    const auto it = op_.find(key);
    if (it == op_.end() || it->is_null())
      return std::optional<Rational>{};
    auto t = parse_time(*it, TimeContext{ctx_.rate});
    if (!t)
      return fail("E_PARAM", "\"" + std::string(key) + "\" is not a time: " + t.error().message,
                  "Write times like \"2.5s\", \"75@30\" or \"00:00:02:15\".");
    return std::optional<Rational>{*t};
  }

  Result<Rational> time_or(const char *key, Rational fallback) const {
    ATM_TRY(auto t, time(key));
    return t ? *t : fallback;
  }

  // ---- writing --------------------------------------------------------------------------------------------------

  // The placeholder of the object this op creates: the op's own "id", or one made from its index.
  std::string placeholder(const std::string &suffix = {}) {
    const std::string id = op_.value("id", std::string());
    const std::string base = id.rfind("$new:", 0) == 0 ? id : "$new:op" + std::to_string(index_);
    const std::string ph = base + suffix;
    out_.names[ph] = ph;
    return ph;
  }

  void push(json op) { out_.ops.push_back(std::move(op)); }

  void add_track_op(const std::string &ph, const std::string &kind, const std::string &name, json anchor) {
    json op = {{"op", "add"}, {"path", ctx_.sequence + "/tracks/" + ph}, {"value", {{"kind", kind}, {"name", name}}}};
    if (!anchor.is_null())
      op["anchor"] = std::move(anchor);
    push(std::move(op));
  }

  std::string next_track_name(const std::string &kind) const {
    int n = 1;
    for (const std::string &id : track_order())
      if (const json *t = track(id); t && (t->value("kind", "video") == "audio") == (kind == "audio"))
        ++n;
    return (kind == "audio" ? "A" : "V") + std::to_string(n);
  }

  // The track a new clip goes on: "track" when given ("new" makes one), else a default chosen by `fallback`.
  // Returns an ID or a placeholder whose creation is already queued.
  Result<std::string> track_for(const std::string &kind, const std::function<Result<std::string>()> &fallback) {
    const auto it = op_.find("track");
    if (it == op_.end() || it->is_null())
      return fallback();
    if (!it->is_string())
      return fail("E_PARAM", "\"track\" must be a track ID, a $new: name, or \"new\".");
    const std::string want = it->get<std::string>();
    if (want == "new") {
      const std::string ph = placeholder(".track");
      add_track_op(ph, kind, op_.value("track_name", next_track_name(kind)), nullptr);
      return ph;
    }
    const json *t = track(want);
    if (!t)
      return fail("E_UNKNOWN_TRACK", "There is no track \"" + want + "\" in this sequence.",
                  "Use a track ID from project.inspect, \"new\", or the $new: name of a track made earlier in this call.");
    if ((t->value("kind", "video") == "audio") != (kind == "audio"))
      return fail("E_TRACK_KIND", "Track " + want + " is a " + t->value("kind", "video") + " track, but this clip needs a " +
                                      kind + " track.",
                  kind == "audio" ? "Sound-only files go on audio tracks: use \"track\": \"new\" or an audio track."
                                  : "Pictures go on video tracks.");
    return want;
  }

  // Opacity keys for fades over a clip of `duration`: up from 0 over fade_in, down to 0 over fade_out.
  Result<json> fade_keys(Rational duration, double full) {
    ATM_TRY(auto fin, time("fade_in"));
    ATM_TRY(auto fout, time("fade_out"));
    if (!fin && !fout)
      return json(nullptr);
    const Rational in = fin ? *fin : Rational(), out = fout ? *fout : Rational();
    if (compare(plus(in, out), duration) > 0)
      return fail("E_FADE_TOO_LONG", "fade_in + fade_out is longer than the clip.",
                  "Keep them together at most " + seconds_text(duration) + " s.");
    json keys = json::object();
    const std::string base = placeholder() + ".fade";
    const auto key = [&](int n, Rational t, double v) {
      keys[base + std::to_string(n)] = {{"t", t.to_string()}, {"v", v}};
    };
    if (in.num() > 0) {
      key(0, Rational(), 0.0);
      key(1, in, full);
    }
    if (out.num() > 0) {
      const Rational from = minus(duration, out);
      if (in.num() == 0 || compare(from, in) != 0)
        key(2, from, full);
      key(3, duration, 0.0);
    }
    return keys;
  }

  // ---- ops ------------------------------------------------------------------------------------------------------

  Result<void> add_track() {
    const std::string kind = op_.value("kind", std::string("video"));
    if (kind != "video" && kind != "audio")
      return fail("E_PARAM", "\"kind\" must be \"video\" or \"audio\".");
    json anchor = nullptr; // default: on top of the others
    const std::string position = op_.value("position", std::string("top"));
    if (op_.contains("below"))
      anchor = {{"before", op_["below"]}};
    else if (op_.contains("above"))
      anchor = {{"after", op_["above"]}};
    else if (position == "bottom")
      anchor = {{"first", true}};
    add_track_op(placeholder(), kind, op_.value("name", next_track_name(kind)), std::move(anchor));
    return {};
  }

  Result<void> add_clip() {
    // The media: an imported asset, or a file path probed now.
    json media;
    std::string asset;
    if (op_.contains("asset")) {
      asset = op_.value("asset", std::string());
      const json &assets = doc_.root().contains("assets") ? doc_.root()["assets"] : json::object();
      if (!assets.contains(asset))
        return fail("E_UNKNOWN_ASSET", "There is no asset \"" + asset + "\".",
                    "Import the file with media.import first, or pass \"path\" instead of \"asset\".");
      media = assets[asset];
    } else if (op_.contains("path")) {
      auto probed = ctx_.probe(op_.value("path", std::string()));
      if (!probed)
        return fail(probed.error().rule, probed.error().message, probed.error().hint);
      media = std::move(*probed);
    } else {
      return fail("E_PARAM", "add_clip needs \"asset\" (from media.import) or \"path\".");
    }
    const bool has_video = media.value("has_video", false);
    const bool image = media.value("image", false); // a still picture: no end, 5 s unless told otherwise
    const std::string kind = has_video ? "video" : "audio";
    std::optional<Rational> media_duration; // none for a still: it has no end
    if (!image)
      if (auto d = Rational::parse(media.value("duration", std::string("0"))))
        media_duration = *d;
    ATM_TRY(Rational source_in, time_or("source_in", Rational()));
    if (source_in.num() < 0)
      return fail("E_PARAM", "\"source_in\" cannot be negative.");
    const Rational rest = media_duration ? minus(*media_duration, source_in) : Rational();
    ATM_TRY(auto dur, time("duration"));
    const Rational duration = dur ? *dur : image ? *Rational::make(5, 1) : rest;
    if (duration.num() <= 0)
      return fail("E_MEDIA_RANGE", "The clip would be empty: the file has " + seconds_text(rest) + " s after source_in.",
                  "Use a smaller source_in or give a positive duration.");
    if (media_duration && compare(plus(source_in, duration), *media_duration) > 0)
      return fail("E_MEDIA_RANGE",
                  "The file is " + seconds_text(*media_duration) + " s long; source_in " + seconds_text(source_in) +
                      " s + duration " + seconds_text(duration) + " s goes past its end.",
                  "Use a duration of at most " + seconds_text(at_most_zero(rest)) + " s, or a smaller source_in.");

    ATM_TRY(std::string track_id, track_for(kind, [&]() -> Result<std::string> {
      for (const std::string &id : track_order()) // the bottom-most track of the right kind
        if (const json *t = track(id); t && (t->value("kind", "video") == "audio") == (kind == "audio") &&
                                       (kind == "audio" || (t->value("name", "") != "Titles" && t->value("name", "") != "Effects")))
          return id;
      const std::string ph = placeholder(".track");
      add_track_op(ph, kind, next_track_name(kind), kind == "video" ? json{{"first", true}} : json(nullptr));
      return ph;
    }));
    ATM_TRY(auto at, time("at"));
    const Rational start = at ? *at : (track(track_id) ? track_end(track_id) : Rational());

    const std::string path = media.value("path", std::string());
    json ref = image ? json{{"type", "image"}, {"path", path}}
                     : json{{"type", "file"}, {"path", path}, {"duration", media.value("duration", std::string("0"))},
                            {"has_audio", media.value("has_audio", false)}};
    if (!asset.empty())
      ref["asset"] = asset;
    if (has_video)
      for (const char *k : {"width", "height", "rate"})
        if (media.contains(k))
          ref[k] = media[k];
    const std::u8string stem = std::filesystem::path(std::u8string(path.begin(), path.end())).stem().u8string();
    const std::string clip_name = op_.value("name", std::string(stem.begin(), stem.end()));
    // A video with sound becomes two linked clips (F1 §5.8): the picture here, the sound on an audio track, so the
    // sound can be cut, faded and mixed on its own while edits keep the two together. with_audio: false keeps only
    // the picture.
    const bool sound = has_video && media.value("has_audio", false) && op_.value("with_audio", true);
    if (has_video && !image)
      ref["stream"] = "video";
    json sound_ref = ref;
    sound_ref["stream"] = "audio";
    for (const char *k : {"width", "height", "rate"})
      sound_ref.erase(k);
    json value = {{"name", clip_name},
                  {"timing", {{"record_in", start.to_string()}, {"duration", duration.to_string()}, {"source_in", source_in.to_string()}}},
                  {"media_ref", std::move(ref)},
                  {"volume", op_.value("volume", 1.0)}};
    if (op_.contains("gain_db") && !has_video)
      value["audio"]["gain_db"] = op_["gain_db"];
    if (has_video) {
      value["transform"] = {{"opacity", op_.value("opacity", 1.0)}};
      for (const char *k : {"position", "scale", "rotation", "anchor", "crop"})
        if (op_.contains(k))
          value["transform"][k] = op_[k];
      ATM_TRY(json keys, fade_keys(duration, op_.value("opacity", 1.0)));
      if (!keys.is_null())
        value["transform"]["keyframes"]["opacity"] = std::move(keys);
    } else { // sound only: fades are sound fades
      ATM_TRY(auto fin, time("fade_in"));
      ATM_TRY(auto fout, time("fade_out"));
      if (fin)
        value["audio"]["fade_in"] = fin->to_string();
      if (fout)
        value["audio"]["fade_out"] = fout->to_string();
    }
    if (sound) {
      const std::string group = new_id("lnk");
      value["link_group"] = group;
      value.erase("volume");
      ATM_TRY(std::string audio_track, sound_track_for(start, duration));
      json audio = {{"name", clip_name},
                    {"timing", value["timing"]},
                    {"media_ref", std::move(sound_ref)},
                    {"volume", op_.value("volume", 1.0)},
                    {"link_group", group}};
      if (op_.contains("gain_db"))
        audio["audio"]["gain_db"] = op_["gain_db"];
      push({{"op", "add"}, {"path", track_id + "/clips/" + placeholder()}, {"value", std::move(value)}});
      push({{"op", "add"}, {"path", audio_track + "/clips/" + placeholder(".audio")}, {"value", std::move(audio)}});
      return {};
    }
    push({{"op", "add"}, {"path", track_id + "/clips/" + placeholder()}, {"value", std::move(value)}});
    return {};
  }

  // The audio track for a clip's linked sound: "audio_track" when given, else the first audio track with room for
  // [start, start + duration), else a new one on top.
  Result<std::string> sound_track_for(Rational start, Rational duration) {
    if (op_.contains("audio_track")) {
      const std::string id = op_.value("audio_track", std::string());
      const json *t = track(id);
      if (!t || t->value("kind", "video") != "audio")
        return fail("E_TRACK_KIND", "\"audio_track\" must be the ID of an audio track.");
      return id;
    }
    const Rational end = plus(start, duration);
    for (const std::string &id : track_order()) {
      const json *t = track(id);
      if (!t || t->value("kind", "video") != "audio")
        continue;
      bool free = true;
      if (t->contains("clips"))
        for (const json &c : (*t)["clips"])
          if (compare(span_of(c).in, end) < 0 && compare(start, span_of(c).end()) < 0)
            free = false;
      if (free)
        return id;
    }
    const std::string ph = placeholder(".audio_track");
    add_track_op(ph, "audio", next_track_name("audio"), nullptr);
    return ph;
  }

  Result<void> add_text() {
    const std::string text = op_.value("text", std::string());
    if (text.empty())
      return fail("E_PARAM", "add_text needs \"text\".", "Lines are separated by \\n.");
    ATM_TRY(std::string track_id, track_for("video", [&]() -> Result<std::string> {
      if (const std::string id = track_id_named("Titles"); !id.empty())
        return id;
      const std::string ph = placeholder(".track");
      add_track_op(ph, "video", "Titles", nullptr); // on top, over the video and any effects
      return ph;
    }));
    ATM_TRY(Rational start, time_or("at", Rational()));
    ATM_TRY(Rational duration, time_or("duration", *Rational::make(3, 1)));
    json position = json::array({0.5, 0.5});
    const std::string placement = op_.value("placement", std::string());
    if (placement == "lower_third")
      position = {0.5, 0.84};
    else if (placement == "top")
      position = {0.5, 0.12};
    else if (placement == "bottom")
      position = {0.5, 0.92};
    if (op_.contains("position"))
      position = op_["position"];
    json value = {{"name", op_.value("name", std::string("Title"))},
                  {"timing", {{"record_in", start.to_string()}, {"duration", duration.to_string()}, {"source_in", "0"}}},
                  {"media_ref", {{"type", "text"}}},
                  {"content",
                   {{"text", text}, {"size", op_.value("size", 0.08)}, {"color", op_.value("color", std::string("#ffffff"))},
                    {"bold", op_.value("bold", true)}}},
                  {"transform", {{"position", position}, {"opacity", op_.value("opacity", 1.0)}}}};
    for (const char *k : {"scale", "rotation", "anchor", "crop"})
      if (op_.contains(k))
        value["transform"][k] = op_[k];
    ATM_TRY(json keys, fade_keys(duration, op_.value("opacity", 1.0)));
    if (!keys.is_null())
      value["transform"]["keyframes"]["opacity"] = std::move(keys);
    push({{"op", "add"}, {"path", track_id + "/clips/" + placeholder()}, {"value", std::move(value)}});
    return {};
  }

  Result<void> add_adjustment() {
    ATM_TRY(std::string track_id, track_for("video", [&]() -> Result<std::string> {
      if (const std::string id = track_id_named("Effects"); !id.empty())
        return id;
      const std::string ph = placeholder(".track");
      const std::string titles = track_id_named("Titles"); // under the titles, so text stays sharp
      add_track_op(ph, "video", "Effects", titles.empty() ? json(nullptr) : json{{"before", titles}});
      return ph;
    }));
    ATM_TRY(Rational start, time_or("at", Rational()));
    ATM_TRY(Rational duration, time_or("duration", *Rational::make(2, 1)));
    json effects = json::object();
    int n = 0;
    const auto add_fx = [&](const eval::EffectDef &def, const json &src) {
      effects[placeholder(".fx" + std::to_string(n++))] = effect_object(def, src);
    };
    const eval::EffectDef &blur_def = *eval::find_effect("gaussian_blur");
    if (op_.contains("blur"))
      add_fx(blur_def, json{{"radius", op_.value("blur", 0.02)}});
    if (const auto list = op_.find("effects"); list != op_.end() && list->is_array())
      for (const json &e : *list) {
        const eval::EffectDef *def = eval::find_effect(e.value("type", std::string("gaussian_blur")));
        if (!def)
          return fail("EFFECT_UNSUPPORTED", "The effect \"" + e.value("type", std::string()) + "\" is not available.",
                      "Use one of: " + eval::effect_ids() + ".");
        add_fx(*def, e);
      }
    if (effects.empty())
      add_fx(blur_def, json::object());
    json value = {{"name", op_.value("name", std::string("Blur"))},
                  {"timing", {{"record_in", start.to_string()}, {"duration", duration.to_string()}, {"source_in", "0"}}},
                  {"media_ref", {{"type", "adjustment"}}},
                  {"effects", std::move(effects)},
                  {"transform", {{"opacity", op_.value("opacity", 1.0)}}}};
    ATM_TRY(json keys, fade_keys(duration, op_.value("opacity", 1.0)));
    if (!keys.is_null())
      value["transform"]["keyframes"]["opacity"] = std::move(keys);
    push({{"op", "add"}, {"path", track_id + "/clips/" + placeholder()}, {"value", std::move(value)}});
    return {};
  }

  Result<void> add_transition() {
    const auto between = op_.find("between");
    if (between == op_.end() || !between->is_array() || between->size() != 2)
      return fail("E_PARAM", "add_transition needs \"between\": [first clip, second clip].");
    const std::string a = (*between)[0].is_string() ? (*between)[0].get<std::string>() : "";
    const std::string b = (*between)[1].is_string() ? (*between)[1].get<std::string>() : "";
    const doc::NodeRef *ra = doc_.find(a), *rb = doc_.find(b);
    if (!ra || !rb || id_prefix(a) != "clp" || id_prefix(b) != "clp")
      return fail("E_UNKNOWN_CLIP", "\"between\" must name two clips.", "Use clip IDs or $new: names of clips made earlier in this call.");
    if (ra->parent != rb->parent)
      return fail("TRANSITION_NOT_ADJACENT", "The two clips are on different tracks.", "A dissolve joins two clips on one track.");
    const Span sa = span_of(*ra->node), sb = span_of(*rb->node);
    if (compare(sa.end(), sb.in) != 0)
      return fail("TRANSITION_NOT_ADJACENT",
                  "The first clip ends at " + seconds_text(sa.end()) + " s but the second starts at " + seconds_text(sb.in) + " s.",
                  "Put the second clip at the first one's end (append, or \"at\": \"" + sa.end().to_string() + "\").");
    const std::string type = op_.value("type", std::string("attome.dissolve"));
    const std::string kind = eval::transition_id(type);
    if (kind.empty())
      return fail("TRANSITION_UNSUPPORTED", "The transition \"" + type + "\" is not available.",
                  "Use one of: " + eval::transition_ids() + ".");
    json params = json::object(); // a wipe or push: the side the incoming clip enters from (a wipe: and its softness)
    if (kind == "wipe" || kind == "push") {
      const std::string direction = op_.value("direction", std::string("left"));
      eval::WipeDirection dir;
      if (!eval::parse_wipe_direction(direction, dir))
        return fail("E_PARAM", "\"direction\" must be left, right, up or down.");
      params = {{"direction", direction}};
      if (kind == "wipe")
        params["softness"] = op_.value("softness", 0.1);
    }
    ATM_TRY(Rational d, time_or("duration", Rational::from_int(1)));
    const std::string alignment = op_.value("alignment", std::string("center"));
    Rational in, out;
    if (alignment == "center") {
      in = *Rational::make(d.num(), d.den() * 2);
      out = minus(d, in);
    } else if (alignment == "start") { // begins at the cut
      out = d;
    } else if (alignment == "end") { // ends at the cut
      in = d;
    } else {
      return fail("E_PARAM", "\"alignment\" must be center, start or end.");
    }
    push({{"op", "add"},
          {"path", ra->parent + "/transitions/" + placeholder()},
          {"value", transition_value(kind, a, b, in, out, params)}});
    int n = 0; // the linked sound clips that meet at the same cut cross-fade over the same range
    for (const auto &[la, lb] : linked_pairs(a, b))
      push({{"op", "add"},
            {"path", doc_.find(la)->parent + "/transitions/" + placeholder(".audio" + std::to_string(n++))},
            {"value", {{"type", "attome.dissolve"}, {"from", la}, {"to", lb}, {"in_offset", in.to_string()}, {"out_offset", out.to_string()}}}});
    return {};
  }

  // ---- links (F1 §5.8: every edit applies to all members of a link_group unless the op says "unlink": true) -------

  struct Member {
    std::string id, track;
  };

  // The other clips of this clip's link group, on any track of the sequence.
  std::vector<Member> linked(const std::string &clip_id) const {
    std::vector<Member> out;
    if (op_.value("unlink", false))
      return out;
    const doc::NodeRef *ref = doc_.find(clip_id);
    const std::string group = ref ? ref->node->value("link_group", std::string()) : std::string();
    if (group.empty())
      return out;
    for (const std::string &tid : track_order())
      if (const json *t = track(tid); t && t->contains("clips"))
        for (auto it = (*t)["clips"].begin(); it != (*t)["clips"].end(); ++it)
          if (it.key() != clip_id && it->value("link_group", std::string()) == group)
            out.push_back({it.key(), tid});
    return out;
  }

  const json &node_of(const std::string &id) const { return *doc_.find(id)->node; }

  // Removes the dissolves that join a clip; a note tells the agent.
  void drop_transitions(const std::string &clip_id, const std::string &track_id) {
    const json *t = track(track_id);
    if (!t || !t->contains("transitions"))
      return;
    for (auto it = (*t)["transitions"].begin(); it != (*t)["transitions"].end(); ++it)
      if (it->value("from", "") == clip_id || it->value("to", "") == clip_id) {
        push({{"op", "remove"}, {"path", it.key()}});
        out_.notes.push_back("Removed dissolve " + it.key() + ": its cut moved. Add it again with add_transition if needed.");
      }
  }

  // The media a clip may use: source_in >= 0 and source_in + duration <= the file's length (unknown or text: no limit).
  Result<void> check_media(const json &clip, const std::string &id, const Span &s) const {
    const json ref = clip.value("media_ref", json::object());
    if (ref.value("type", "") != "file")
      return {};
    if (s.source_in.num() < 0)
      return fail("E_MEDIA_RANGE", "Clip " + id + " would start " + seconds_text(minus(Rational(), s.source_in)) +
                                       " s before the beginning of its file.",
                  "Move by less; source_in cannot go below 0.");
    if (const auto total = Rational::parse(ref.value("duration", std::string()));
        total && total->num() > 0 && compare(plus(s.source_in, s.duration), *total) > 0)
      return fail("E_MEDIA_RANGE", "Clip " + id + " would run " + seconds_text(minus(plus(s.source_in, s.duration), *total)) +
                                       " s past the end of its file.",
                  "Move by less; the file is " + seconds_text(*total) + " s long.");
    return {};
  }

  void put_span(const std::string &id, const Span &before, const Span &after) {
    if (compare(after.in, before.in) != 0)
      push({{"op", "replace"}, {"path", id + "/timing/record_in"}, {"value", after.in.to_string()}});
    if (compare(after.duration, before.duration) != 0)
      push({{"op", "replace"}, {"path", id + "/timing/duration"}, {"value", after.duration.to_string()}});
    if (compare(after.source_in, before.source_in) != 0)
      push({{"op", "replace"}, {"path", id + "/timing/source_in"}, {"value", after.source_in.to_string()}});
  }

  // A clip on the same track that ends (ending = true) or starts exactly at `t`.
  std::string neighbour(const std::string &track_id, const std::string &self, Rational t, bool ending) const {
    if (const json *tr = track(track_id); tr && tr->contains("clips"))
      for (auto it = (*tr)["clips"].begin(); it != (*tr)["clips"].end(); ++it)
        if (it.key() != self && compare(ending ? span_of(*it).end() : span_of(*it).in, t) == 0)
          return it.key();
    return {};
  }

  // ---- edits ----------------------------------------------------------------------------------------------------

  Result<void> remove(bool ripple) {
    if (op_.contains("transition") && !ripple) { // a dissolve on its own
      const std::string id = op_.value("transition", std::string());
      if (!doc_.find(id) || id_prefix(id) != "trn")
        return fail("E_UNKNOWN_ID", "\"transition\" must be the ID of a dissolve, not \"" + id + "\".");
      push({{"op", "remove"}, {"path", id}});
      return {};
    }
    ATM_TRY(auto c, clip("clip"));
    std::vector<Member> all = linked(op_.value("clip", std::string()));
    all.insert(all.begin(), {op_.value("clip", std::string()), c.second});
    for (const Member &m : all) {
      drop_transitions(m.id, m.track);
      push({{"op", "remove"}, {"path", m.id}});
    }
    if (ripple) // on each track, the later clips move left by the gap
      for (const Member &m : all) {
        const Span s = span_of(node_of(m.id));
        for (auto it = (*track(m.track))["clips"].begin(); it != (*track(m.track))["clips"].end(); ++it) {
          const bool gone = std::any_of(all.begin(), all.end(), [&](const Member &x) { return x.id == it.key(); });
          if (!gone && compare(span_of(*it).in, s.in) > 0)
            push({{"op", "replace"}, {"path", it.key() + "/timing/record_in"}, {"value", minus(span_of(*it).in, s.duration).to_string()}});
        }
      }
    return {};
  }

  Result<void> move() {
    ATM_TRY(auto c, clip("clip"));
    const std::string id = op_.value("clip", std::string());
    ATM_TRY(auto to, time("to"));
    if (!to && !op_.contains("track"))
      return fail("E_PARAM", "move needs \"to\" (a time) and/or \"track\".");
    drop_transitions(id, c.second);
    if (op_.contains("track") && op_.value("track", std::string()) != c.second) {
      const std::string dest = op_.value("track", std::string());
      if (!track(dest))
        return fail("E_UNKNOWN_TRACK", "There is no track \"" + dest + "\".");
      push({{"op", "move"}, {"path", id}, {"to", dest + "/clips"}});
    }
    if (to) {
      const Rational delta = minus(*to, span_of(*c.first).in);
      push({{"op", "replace"}, {"path", id + "/timing/record_in"}, {"value", to->to_string()}});
      for (const Member &m : linked(id)) { // the linked clips move by the same amount, on their own tracks
        drop_transitions(m.id, m.track);
        push({{"op", "replace"}, {"path", m.id + "/timing/record_in"}, {"value", plus(span_of(node_of(m.id)).in, delta).to_string()}});
      }
    }
    return {};
  }

  // Moves one clip's edge by `d` (in: start and source move together; out: the end moves).
  Result<void> trim_one(const std::string &id, const std::string &track_id, bool in_edge, Rational d) {
    const Span s = span_of(node_of(id));
    Span n = s;
    if (in_edge)
      n = {plus(s.in, d), minus(s.duration, d), plus(s.source_in, d)};
    else
      n.duration = plus(s.duration, d);
    if (n.duration.num() <= 0)
      return fail("E_MEDIA_RANGE", "Clip " + id + " would be empty.", "Trim by less than its duration, " + seconds_text(s.duration) + " s.");
    ATM_CHECK(check_media(node_of(id), id, n));
    drop_transitions(id, track_id);
    put_span(id, s, n);
    return {};
  }

  Result<void> trim() {
    ATM_TRY(auto c, clip("clip"));
    const std::string id = op_.value("clip", std::string());
    const std::string edge = op_.value("edge", std::string("out"));
    if (edge != "in" && edge != "out")
      return fail("E_PARAM", "\"edge\" must be \"in\" or \"out\".");
    const Span s = span_of(*c.first);
    ATM_TRY(auto to, time("to"));
    ATM_TRY(auto delta, time("delta"));
    if (!to && !delta)
      return fail("E_PARAM", "trim needs \"to\" (where the edge goes) or \"delta\" (how far it moves).");
    const bool in_edge = edge == "in";
    const Rational d = delta ? *delta : minus(*to, in_edge ? s.in : s.end());
    ATM_CHECK(trim_one(id, c.second, in_edge, d));
    for (const Member &m : linked(id)) // the same edge of the linked clips moves the same way
      ATM_CHECK(trim_one(m.id, m.track, in_edge, d));
    return {};
  }

  // Cuts one clip at `at`; the right half is `right_ph`, in `group` (empty: no group). Keyframes become local to each
  // half and effects get new IDs (and their parameters' keyframes become local to the half).
  void split_one(const std::string &id, const std::string &track_id, Rational at, const std::string &right_ph,
                 const std::string &group) {
    const json &node = node_of(id);
    const Span s = span_of(node);
    const Rational left = minus(at, s.in);
    json right = node;
    right["timing"] = {{"record_in", at.to_string()}, {"duration", minus(s.duration, left).to_string()},
                       {"source_in", plus(s.source_in, left).to_string()}};
    if (group.empty())
      right.erase("link_group");
    else
      right["link_group"] = group;
    int n = 0;
    if (right.contains("transform") && right["transform"].contains("keyframes"))
      for (auto &[prop, keys] : right["transform"]["keyframes"].items()) {
        json moved = json::object();
        for (const auto &[kid, key] : keys.items()) {
          json k = key;
          if (const auto t = Rational::parse(k.value("t", std::string("0"))))
            k["t"] = minus(*t, left).to_string();
          moved[right_ph + ".k" + std::to_string(n++)] = std::move(k);
        }
        keys = std::move(moved);
      }
    if (right.contains("effects")) {
      json fx = json::object();
      for (const auto &[fid, e] : right["effects"].items()) {
        json moved_fx = e;
        if (moved_fx.contains("keyframes") && moved_fx["keyframes"].is_object()) // the keys of the parameters, local to the half
          for (auto &[param, keys] : moved_fx["keyframes"].items()) {
            json moved = json::object();
            for (const auto &[kid, key] : keys.items()) {
              json k = key;
              if (const auto t = Rational::parse(k.value("t", std::string("0"))))
                k["t"] = minus(*t, left).to_string();
              moved[right_ph + ".k" + std::to_string(n++)] = std::move(k);
            }
            keys = std::move(moved);
          }
        fx[right_ph + ".fx" + std::to_string(n++)] = std::move(moved_fx);
      }
      right["effects"] = std::move(fx);
      right.erase("effect_order");
    }
    push({{"op", "replace"}, {"path", id + "/timing/duration"}, {"value", left.to_string()}});
    push({{"op", "add"}, {"path", track_id + "/clips/" + right_ph}, {"anchor", {{"after", id}}}, {"value", std::move(right)}});
    if (const json *t = track(track_id); t && t->contains("transitions")) // a dissolve out of the clip leaves from the right half
      for (auto it = (*t)["transitions"].begin(); it != (*t)["transitions"].end(); ++it)
        if (it->value("from", "") == id)
          push({{"op", "replace"}, {"path", it.key() + "/from"}, {"value", right_ph}});
  }

  Result<void> split() {
    ATM_TRY(auto c, clip("clip"));
    const std::string id = op_.value("clip", std::string());
    ATM_TRY(auto at, time("at"));
    const Span s = span_of(*c.first);
    if (!at || compare(*at, s.in) <= 0 || compare(*at, s.end()) >= 0)
      return fail("E_PARAM", "\"at\" must be a time inside the clip, between " + seconds_text(s.in) + " and " + seconds_text(s.end()) + " s.");
    // Linked clips that span the cut are cut too; the right halves form a new group of their own.
    std::vector<Member> cut;
    for (const Member &m : linked(id)) {
      const Span ms = span_of(node_of(m.id));
      if (compare(*at, ms.in) > 0 && compare(*at, ms.end()) < 0)
        cut.push_back(m);
    }
    const std::string group = cut.empty() ? std::string() : new_id("lnk");
    const std::string base = placeholder();
    split_one(id, c.second, *at, base, group);
    for (size_t i = 0; i < cut.size(); ++i)
      split_one(cut[i].id, cut[i].track, *at, placeholder(".linked" + std::to_string(i)), group);
    return {};
  }

  // slip: the clip stays where it is on the timeline and shows another part of its file.
  Result<void> slip() {
    ATM_TRY(auto c, clip("clip"));
    const std::string id = op_.value("clip", std::string());
    const Span s = span_of(*c.first);
    ATM_TRY(auto to, time("source_in"));
    ATM_TRY(auto delta, time("delta"));
    if (!to && !delta)
      return fail("E_PARAM", "slip needs \"delta\" (how far into the file to move) or \"source_in\".");
    const Rational d = delta ? *delta : minus(*to, s.source_in);
    std::vector<Member> all = linked(id);
    all.insert(all.begin(), {id, c.second});
    for (const Member &m : all) { // picture and sound stay in sync
      const Span ms = span_of(node_of(m.id));
      Span n = ms;
      n.source_in = plus(ms.source_in, d);
      ATM_CHECK(check_media(node_of(m.id), m.id, n));
      put_span(m.id, ms, n);
    }
    return {};
  }

  // roll: the cut between two touching clips moves; the first gets longer as the second gets shorter, or the reverse.
  Result<void> roll_pair(const std::string &a, const std::string &b, Rational d) {
    const Span sa = span_of(node_of(a)), sb = span_of(node_of(b));
    Span na = sa;
    na.duration = plus(sa.duration, d);
    const Span nb = {plus(sb.in, d), minus(sb.duration, d), plus(sb.source_in, d)};
    if (na.duration.num() <= 0 || nb.duration.num() <= 0)
      return fail("E_MEDIA_RANGE", "The cut would move past the start of the first clip or the end of the second.",
                  "Move it by less than " + seconds_text(d.num() < 0 ? sa.duration : sb.duration) + " s.");
    ATM_CHECK(check_media(node_of(a), a, na));
    ATM_CHECK(check_media(node_of(b), b, nb));
    put_span(a, sa, na);
    put_span(b, sb, nb);
    return {};
  }

  // The linked partners of a and b that meet at the same cut on one track (the sound of two linked clips).
  std::vector<std::pair<std::string, std::string>> linked_pairs(const std::string &a, const std::string &b) const {
    std::vector<std::pair<std::string, std::string>> out;
    const Rational cut = span_of(node_of(b)).in;
    for (const Member &ma : linked(a))
      for (const Member &mb : linked(b))
        if (ma.track == mb.track && compare(span_of(node_of(ma.id)).end(), cut) == 0 && compare(span_of(node_of(mb.id)).in, cut) == 0)
          out.emplace_back(ma.id, mb.id);
    return out;
  }

  Result<void> roll() {
    const auto between = op_.find("between");
    if (between == op_.end() || !between->is_array() || between->size() != 2 || !(*between)[0].is_string() ||
        !(*between)[1].is_string())
      return fail("E_PARAM", "roll needs \"between\": [first clip, second clip] and \"delta\" (or \"to\", the new cut).");
    const std::string a = (*between)[0], b = (*between)[1];
    const doc::NodeRef *ra = doc_.find(a), *rb = doc_.find(b);
    if (!ra || !rb || ra->parent != rb->parent)
      return fail("E_UNKNOWN_CLIP", "\"between\" must name two clips on one track.");
    const Span sa = span_of(*ra->node), sb = span_of(*rb->node);
    if (compare(sa.end(), sb.in) != 0)
      return fail("E_NOT_ADJACENT", "The clips do not touch: the first ends at " + seconds_text(sa.end()) +
                                        " s, the second starts at " + seconds_text(sb.in) + " s.");
    ATM_TRY(auto to, time("to"));
    ATM_TRY(auto delta, time("delta"));
    if (!to && !delta)
      return fail("E_PARAM", "roll needs \"delta\" or \"to\".");
    const Rational d = to ? minus(*to, sb.in) : *delta;
    const auto pairs = linked_pairs(a, b); // read before any change
    ATM_CHECK(roll_pair(a, b, d));
    for (const auto &[la, lb] : pairs)
      ATM_CHECK(roll_pair(la, lb, d));
    return {};
  }

  // slide: the clip moves along the track between its neighbours; the one before gets longer (or shorter) and the
  // one after gives up (or gains) the same time, so the rest of the timeline stays put.
  Result<void> slide_one(const std::string &id, const std::string &track_id, Rational delta) {
    const Span s = span_of(node_of(id));
    const std::string before = neighbour(track_id, id, s.in, true), after = neighbour(track_id, id, s.end(), false);
    Span n = s;
    n.in = plus(s.in, delta);
    if (!before.empty()) {
      const Span sp = span_of(node_of(before));
      Span np = sp;
      np.duration = plus(sp.duration, delta);
      if (np.duration.num() <= 0)
        return fail("E_MEDIA_RANGE", "Sliding by " + seconds_text(delta) + " s would empty the clip before it.");
      ATM_CHECK(check_media(node_of(before), before, np));
      put_span(before, sp, np);
    }
    if (!after.empty()) {
      const Span sn = span_of(node_of(after));
      const Span nn = {plus(sn.in, delta), minus(sn.duration, delta), plus(sn.source_in, delta)};
      if (nn.duration.num() <= 0)
        return fail("E_MEDIA_RANGE", "Sliding by " + seconds_text(delta) + " s would empty the clip after it.");
      ATM_CHECK(check_media(node_of(after), after, nn));
      put_span(after, sn, nn);
    }
    put_span(id, s, n);
    return {};
  }

  Result<void> slide() {
    ATM_TRY(auto c, clip("clip"));
    const std::string id = op_.value("clip", std::string());
    ATM_TRY(auto delta, time("delta"));
    if (!delta)
      return fail("E_PARAM", "slide needs \"delta\".");
    const std::vector<Member> members = linked(id);
    ATM_CHECK(slide_one(id, c.second, *delta));
    for (const Member &m : members)
      ATM_CHECK(slide_one(m.id, m.track, *delta));
    return {};
  }

  // link {clips: [...]}: one group, so edits move them together. unlink {clip}: the clip leaves its group.
  Result<void> link() {
    const auto clips = op_.find("clips");
    if (clips == op_.end() || !clips->is_array() || clips->size() < 2)
      return fail("E_PARAM", "link needs \"clips\": two or more clip IDs.");
    const std::string group = new_id("lnk");
    for (const json &c : *clips) {
      const std::string id = c.is_string() ? c.get<std::string>() : std::string();
      if (!doc_.find(id) || id_prefix(id) != "clp")
        return fail("E_UNKNOWN_CLIP", "\"" + id + "\" is not a clip.");
      push({{"op", "replace"}, {"path", id + "/link_group"}, {"value", group}});
    }
    return {};
  }

  Result<void> unlink() {
    ATM_TRY(auto c, clip("clip"));
    const std::string id = op_.value("clip", std::string());
    if (!c.first->contains("link_group"))
      return fail("E_NOT_LINKED", "Clip " + id + " is not linked to another clip.");
    push({{"op", "remove"}, {"path", id + "/link_group"}});
    const std::vector<Member> rest = linked(id);
    if (rest.size() == 1) // a group of one is no group
      push({{"op", "remove"}, {"path", rest[0].id + "/link_group"}});
    return {};
  }

  // add_effect: a blur on one clip (or on an adjustment layer); it changes only that clip.
  Result<void> add_effect() {
    const std::string target = op_.value("target", op_.value("clip", std::string()));
    const doc::NodeRef *ref = target.empty() ? nullptr : doc_.find(target);
    if (!ref || id_prefix(target) != "clp")
      return fail("E_UNKNOWN_CLIP", "\"target\" must be a clip ID or a $new: name.");
    const std::string type = op_.value("type", std::string("gaussian_blur"));
    const eval::EffectDef *def = eval::find_effect(type);
    if (!def)
      return fail("EFFECT_UNSUPPORTED", "The effect \"" + type + "\" is not available.", "Use one of: " + eval::effect_ids() + ".");
    json src = op_; // the parameters sit on the op itself ("radius": 0.02) or in "params"
    if (const auto p = op_.find("params"); p != op_.end() && p->is_object())
      src.update(*p);
    json value = effect_object(*def, src);
    value["enabled"] = op_.value("enabled", true);
    push({{"op", "add"}, {"path", target + "/effects/" + placeholder()}, {"value", std::move(value)}});
    return {};
  }

  Result<void> change_effect(bool remove) {
    const std::string id = op_.value("effect", std::string());
    if (id.empty() || !doc_.find(id) || id_prefix(id) != "fx")
      return fail("E_UNKNOWN_ID", "\"effect\" must be the ID of an effect (fx_…).",
                  "Read a clip's effects with project.get, or use the $new: name given to add_effect.");
    if (remove)
      push({{"op", "remove"}, {"path", id}});
    else
      push({{"op", "replace"}, {"path", id + "/enabled"}, {"value", op_.value("enabled", true)}});
    return {};
  }

  Result<void> set_property() {
    const std::string target = op_.value("target", std::string());
    const doc::NodeRef *ref = target.empty() ? nullptr : doc_.find(target);
    if (!ref)
      return fail("E_UNKNOWN_ID", "\"target\" must be the ID of a clip, track or effect.");
    std::string path = op_.value("path", std::string());
    if (path.empty())
      return fail("E_PARAM", "set_property needs \"path\", e.g. \"transform.opacity\" or \"audio.gain_db\".");
    for (const char *blocked : {"timing", "media_ref", "keyframes", "transitions", "clips", "effects"})
      if (path == blocked || path.rfind(std::string(blocked) + ".", 0) == 0)
        return fail("E_PARAM", "\"" + path + "\" is not changed by set_property.",
                    "Use move or trim for timing, add_transition for dissolves, and \"keyframes\" with a transform path "
                    "for animation. To change an effect, target its ID (fx_…) with path \"params.radius\".");
    std::replace(path.begin(), path.end(), '.', '/');
    if (const auto keys = op_.find("keyframes"); keys != op_.end()) {
      if (!keys->is_array())
        return fail("E_PARAM", "\"keyframes\" must be an array of {t, v, interp?, ease?}.");
      if (id_prefix(target) == "fx") { // an effect parameter: path "params.<name>"
        const auto def = eval::find_effect(ref->node->value("effect", std::string()));
        const std::string param = path.rfind("params/", 0) == 0 ? path.substr(7) : std::string();
        const bool known = def && std::any_of(def->params.begin(), def->params.end(),
                                              [&](const eval::EffectParam &p) { return param == p.key; });
        if (!known)
          return fail("E_PARAM", "Keyframes of an effect go on one of its parameters, path \"params.<name>\".",
                      def ? "The parameters of this effect are in guide.get topic \"effects\"." : "The target is not a known effect.");
        if (const auto old = ref->node->find("keyframes"); old != ref->node->end() && old->contains(param)) // the new keys replace the old
          for (const auto &[kid, k] : (*old)[param].items())
            push({{"op", "remove"}, {"path", kid}});
        const std::string base = placeholder();
        int n = 0;
        for (const json &k : *keys)
          push({{"op", "add"}, {"path", target + "/keyframes/" + param + "/" + base + ".k" + std::to_string(n++)}, {"value", k}});
        return {};
      }
      const std::string prop = path.rfind("transform/", 0) == 0 ? path.substr(10) : std::string();
      if (prop != "opacity" && prop != "position" && prop != "scale" && prop != "rotation" && prop != "anchor")
        return fail("E_PARAM", "Keyframes go on transform.opacity, position, scale, rotation or anchor, or on an effect's params.",
                    "Crop cannot be animated yet; set it with path \"transform.crop\" and a value.");
      // The new keys replace the old ones of this property.
      if (const auto tr = ref->node->find("transform"); tr != ref->node->end() && tr->contains("keyframes") &&
                                                        (*tr)["keyframes"].contains(prop))
        for (const auto &[kid, k] : (*tr)["keyframes"][prop].items())
          push({{"op", "remove"}, {"path", kid}});
      const std::string base = placeholder();
      int n = 0;
      for (const json &k : *keys)
        push({{"op", "add"}, {"path", target + "/transform/keyframes/" + prop + "/" + base + ".k" + std::to_string(n++)}, {"value", k}});
      return {};
    }
    if (!op_.contains("value"))
      return fail("E_PARAM", "set_property needs \"value\" (or \"keyframes\" for a transform property).");
    push({{"op", "replace"}, {"path", target + "/" + path}, {"value", op_["value"]}});
    return {};
  }
};

} // namespace

Result<Built> build(const Document &doc, const json &op, size_t index, const Context &ctx) {
  if (!op.is_object() || !op.contains("op"))
    return fail(ErrorCode::InvalidArgument, "E_OP", "ops[" + std::to_string(index) + "] needs \"op\".", {},
                "Each op is an object such as {\"op\": \"add_clip\", \"path\": \"…\"}.");
  return Builder(doc, op, index, ctx).run();
}

json sequence_duration(const Document &doc, const Context &ctx) {
  Rational end;
  const json &seq = doc.root()["sequences"][ctx.sequence];
  if (seq.contains("tracks"))
    for (const json &t : seq["tracks"])
      if (t.contains("clips"))
        for (const json &c : t["clips"])
          if (compare(span_of(c).end(), end) > 0)
            end = span_of(c).end();
  return to_json(format_time(end, ctx.rate));
}

} // namespace atm::api::timeline
