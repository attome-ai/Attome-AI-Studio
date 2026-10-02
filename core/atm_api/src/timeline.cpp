#include "timeline.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <vector>

#include "atm/base/id.hpp"
#include "atm/base/time.hpp"

namespace atm::api::timeline {
namespace {

using doc::Document;

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
    return fail("E_OP", "\"" + name + "\" is not a timeline op.",
                "Use add_track, add_clip, add_text, add_adjustment, add_transition, delete, ripple_delete, move, trim, "
                "split or set_property (guide.get topic \"timeline\").");
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
                  "Keep them together at most " + duration.to_string() + " s.");
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
    const std::string kind = has_video ? "video" : "audio";
    const auto media_duration = Rational::parse(media.value("duration", std::string("0")));
    ATM_TRY(Rational source_in, time_or("source_in", Rational()));
    if (source_in.num() < 0)
      return fail("E_PARAM", "\"source_in\" cannot be negative.");
    const Rational rest = media_duration ? minus(*media_duration, source_in) : Rational();
    ATM_TRY(auto dur, time("duration"));
    const Rational duration = dur ? *dur : rest;
    if (duration.num() <= 0)
      return fail("E_MEDIA_RANGE", "The clip would be empty: the file has " + rest.to_string() + " s after source_in.",
                  "Use a smaller source_in or give a positive duration.");
    if (media_duration && compare(plus(source_in, duration), *media_duration) > 0)
      return fail("E_MEDIA_RANGE",
                  "The file is " + media_duration->to_string() + " s long; source_in " + source_in.to_string() +
                      " + duration " + duration.to_string() + " goes past its end.",
                  "Use a duration of at most " + at_most_zero(rest).to_string() + " s, or a smaller source_in.");

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
    json ref = {{"type", "file"}, {"path", path}, {"duration", media.value("duration", std::string("0"))},
                {"has_audio", media.value("has_audio", false)}};
    if (!asset.empty())
      ref["asset"] = asset;
    if (has_video)
      for (const char *k : {"width", "height", "rate"})
        if (media.contains(k))
          ref[k] = media[k];
    const std::u8string stem = std::filesystem::path(std::u8string(path.begin(), path.end())).stem().u8string();
    json value = {{"name", op_.value("name", std::string(stem.begin(), stem.end()))},
                  {"timing", {{"record_in", start.to_string()}, {"duration", duration.to_string()}, {"source_in", source_in.to_string()}}},
                  {"media_ref", std::move(ref)},
                  {"volume", op_.value("with_audio", true) ? op_.value("volume", 1.0) : 0.0}};
    if (op_.contains("gain_db"))
      value["audio"]["gain_db"] = op_["gain_db"];
    if (has_video) {
      value["transform"] = {{"opacity", op_.value("opacity", 1.0)}};
      for (const char *k : {"position", "scale"})
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
    push({{"op", "add"}, {"path", track_id + "/clips/" + placeholder()}, {"value", std::move(value)}});
    return {};
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
    const auto add_blur = [&](double radius) {
      effects[placeholder(".fx" + std::to_string(n++))] = {
          {"effect", "attome.gaussian_blur@1.0.0"}, {"enabled", true}, {"params", {{"radius", radius}}}};
    };
    if (op_.contains("blur"))
      add_blur(op_.value("blur", 0.02));
    if (const auto list = op_.find("effects"); list != op_.end() && list->is_array())
      for (const json &e : *list) {
        const std::string type = e.value("type", std::string("gaussian_blur"));
        if (type != "gaussian_blur" && type != "blur" && type != "attome.gaussian_blur")
          return fail("EFFECT_UNSUPPORTED", "The effect \"" + type + "\" is not available.", "Use gaussian_blur, the one effect so far.");
        add_blur(e.value("radius", 0.02));
      }
    if (effects.empty())
      add_blur(0.02);
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
                  "The first clip ends at " + sa.end().to_string() + " s but the second starts at " + sb.in.to_string() + " s.",
                  "Put the second clip at the first one's end (append, or \"at\": \"" + sa.end().to_string() + "s\").");
    const std::string type = op_.value("type", std::string("attome.dissolve"));
    if (type != "attome.dissolve" && type != "dissolve")
      return fail("TRANSITION_UNSUPPORTED", "The transition \"" + type + "\" is not available.", "Use \"dissolve\".");
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
          {"value", {{"type", "attome.dissolve"}, {"from", a}, {"to", b}, {"in_offset", in.to_string()}, {"out_offset", out.to_string()}}}});
    return {};
  }

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

  Result<void> remove(bool ripple) {
    if (op_.contains("transition") && !ripple) { // a dissolve on its own
      const std::string id = op_.value("transition", std::string());
      if (!doc_.find(id) || id_prefix(id) != "trn")
        return fail("E_UNKNOWN_ID", "\"transition\" must be the ID of a dissolve, not \"" + id + "\".");
      push({{"op", "remove"}, {"path", id}});
      return {};
    }
    ATM_TRY(auto c, clip("clip"));
    const std::string id = op_.value("clip", std::string());
    const Span s = span_of(*c.first);
    drop_transitions(id, c.second);
    push({{"op", "remove"}, {"path", id}});
    if (ripple) // later clips on the track move left by the gap
      for (auto it = (*track(c.second))["clips"].begin(); it != (*track(c.second))["clips"].end(); ++it)
        if (it.key() != id && compare(span_of(*it).in, s.in) > 0)
          push({{"op", "replace"}, {"path", it.key() + "/timing/record_in"}, {"value", minus(span_of(*it).in, s.duration).to_string()}});
    return {};
  }

  Result<void> move() {
    ATM_TRY(auto c, clip("clip"));
    const std::string id = op_.value("clip", std::string());
    ATM_TRY(auto to, time("to"));
    drop_transitions(id, c.second);
    if (op_.contains("track") && op_.value("track", std::string()) != c.second) {
      const std::string dest = op_.value("track", std::string());
      if (!track(dest))
        return fail("E_UNKNOWN_TRACK", "There is no track \"" + dest + "\".");
      push({{"op", "move"}, {"path", id}, {"to", dest + "/clips"}});
    }
    if (to)
      push({{"op", "replace"}, {"path", id + "/timing/record_in"}, {"value", to->to_string()}});
    if (!to && !op_.contains("track"))
      return fail("E_PARAM", "move needs \"to\" (a time) and/or \"track\".");
    return {};
  }

  Result<void> trim() {
    ATM_TRY(auto c, clip("clip"));
    const std::string id = op_.value("clip", std::string());
    const std::string edge = op_.value("edge", std::string("out"));
    const Span s = span_of(*c.first);
    ATM_TRY(auto to, time("to"));
    ATM_TRY(auto delta, time("delta"));
    if (!to && !delta)
      return fail("E_PARAM", "trim needs \"to\" (where the edge goes) or \"delta\" (how far it moves).");
    Span n = s;
    if (edge == "out") {
      const Rational end = to ? *to : plus(s.end(), *delta);
      n.duration = minus(end, s.in);
    } else if (edge == "in") {
      const Rational in = to ? *to : plus(s.in, *delta);
      const Rational d = minus(in, s.in);
      n = {in, minus(s.duration, d), plus(s.source_in, d)};
    } else {
      return fail("E_PARAM", "\"edge\" must be \"in\" or \"out\".");
    }
    if (n.duration.num() <= 0)
      return fail("E_MEDIA_RANGE", "The clip would be empty.", "Trim by less than its duration, " + s.duration.to_string() + " s.");
    const json ref = c.first->value("media_ref", json::object());
    if (ref.value("type", "") == "file") {
      if (n.source_in.num() < 0)
        return fail("E_MEDIA_RANGE", "The file starts " + s.source_in.to_string() + " s before this clip's start; the in edge cannot go earlier.");
      if (const auto total = Rational::parse(ref.value("duration", std::string()));
          total && total->num() > 0 && compare(plus(n.source_in, n.duration), *total) > 0)
        return fail("E_MEDIA_RANGE", "The file ends " + minus(*total, plus(s.source_in, s.duration)).to_string() +
                                         " s after this clip's end; the out edge cannot go later.");
    }
    drop_transitions(id, c.second);
    push({{"op", "replace"}, {"path", id + "/timing/record_in"}, {"value", n.in.to_string()}});
    push({{"op", "replace"}, {"path", id + "/timing/duration"}, {"value", n.duration.to_string()}});
    push({{"op", "replace"}, {"path", id + "/timing/source_in"}, {"value", n.source_in.to_string()}});
    return {};
  }

  Result<void> split() {
    ATM_TRY(auto c, clip("clip"));
    const std::string id = op_.value("clip", std::string());
    ATM_TRY(auto at, time("at"));
    const Span s = span_of(*c.first);
    if (!at || compare(*at, s.in) <= 0 || compare(*at, s.end()) >= 0)
      return fail("E_PARAM", "\"at\" must be a time inside the clip, between " + s.in.to_string() + " and " + s.end().to_string() + " s.");
    const Rational left = minus(*at, s.in);
    json right = *c.first;
    right["timing"] = {{"record_in", at->to_string()}, {"duration", minus(s.duration, left).to_string()},
                       {"source_in", plus(s.source_in, left).to_string()}};
    // New IDs for the copied keyframes and effects; keyframe times become local to the right half.
    int n = 0;
    const std::string base = placeholder();
    if (right.contains("transform") && right["transform"].contains("keyframes"))
      for (auto &[prop, keys] : right["transform"]["keyframes"].items()) {
        json moved = json::object();
        for (const auto &[kid, key] : keys.items()) {
          json k = key;
          if (const auto t = Rational::parse(k.value("t", std::string("0"))))
            k["t"] = minus(*t, left).to_string();
          moved[base + ".k" + std::to_string(n++)] = std::move(k);
        }
        keys = std::move(moved);
      }
    if (right.contains("effects")) {
      json fx = json::object();
      for (const auto &[fid, e] : right["effects"].items())
        fx[base + ".fx" + std::to_string(n++)] = e;
      right["effects"] = std::move(fx);
      right.erase("effect_order");
    }
    push({{"op", "replace"}, {"path", id + "/timing/duration"}, {"value", left.to_string()}});
    push({{"op", "add"}, {"path", c.second + "/clips/" + base}, {"anchor", {{"after", id}}}, {"value", std::move(right)}});
    if (const json *t = track(c.second); t && t->contains("transitions")) // a dissolve out of the clip leaves from the right half
      for (auto it = (*t)["transitions"].begin(); it != (*t)["transitions"].end(); ++it)
        if (it->value("from", "") == id)
          push({{"op", "replace"}, {"path", it.key() + "/from"}, {"value", base}});
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
      const std::string prop = path.rfind("transform/", 0) == 0 ? path.substr(10) : std::string();
      if (prop != "opacity" && prop != "position" && prop != "scale")
        return fail("E_PARAM", "Keyframes go on transform.opacity, transform.position or transform.scale.");
      if (!keys->is_array())
        return fail("E_PARAM", "\"keyframes\" must be an array of {t, v, interp?, ease?}.");
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
