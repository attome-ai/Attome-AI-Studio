#include "timeline.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <vector>

#include "atm/base/id.hpp"
#include "atm/base/time.hpp"
#include "atm/eval/effects.hpp"
#include "atm/gen/library.hpp"

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
  // A file parameter (a LUT's .cube) has no default: left out when not given, which the validator then refuses by name.
  if (def.file_param[0] != '\0' && src.is_object())
    if (const auto f = src.find(def.file_param); f != src.end() && f->is_string())
      params[def.file_param] = *f;
  return {{"effect", eval::effect_name(def)}, {"enabled", true}, {"params", std::move(params)}};
}

// A transition object ("dissolve", "wipe", "push", "zoom", "slide" or "iris") between two clips; the others carry their params.
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
    if (name == "add_captions")
      return wrap(add_captions());
    if (name == "sync_captions")
      return wrap(sync_captions());
    if (name == "add_transition")
      return wrap(add_transition());
    if (name == "make_room")
      return wrap(make_room());
    if (name == "delete" || name == "ripple_delete")
      return wrap(remove(name == "ripple_delete"));
    if (name == "move")
      return wrap(move());
    if (name == "trim")
      return wrap(trim());
    if (name == "split")
      return wrap(split());
    if (name == "duplicate")
      return wrap(duplicate());
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
                "Use add_track, add_clip, add_text, add_captions, sync_captions, add_adjustment, add_transition, make_room, delete, ripple_delete, move, trim, "
                "split, duplicate, slip, roll, slide, add_effect, remove_effect, set_effect_enabled, link, unlink or "
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

  void add_track_op(const std::string &ph, const std::string &kind, const std::string &name, json anchor, bool sync_lock = false) {
    json value = {{"kind", kind}, {"name", name}};
    if (sync_lock)
      value["sync_lock"] = true;
    json op = {{"op", "add"}, {"path", ctx_.sequence + "/tracks/" + ph}, {"value", std::move(value)}};
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
    const auto lock = op_.find("sync_lock");
    if (lock != op_.end() && !lock->is_boolean())
      return fail("E_PARAM", "\"sync_lock\" must be true or false.");
    add_track_op(placeholder(), kind, op_.value("name", next_track_name(kind)), std::move(anchor), lock != op_.end() && lock->get<bool>());
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
      add_track_op(ph, "video", "Titles", nullptr, true); // on top, over the video and any effects; locked to the cut
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

  // add_captions: words said over a stretch of time, as captions: one text clip for each sentence, drawn one word at a time (content.words
  // holds each word and when it starts, after the clip's start), every word popping in. The words are timed in proportion to their letters,
  // with a pause after a comma, a colon and a full stop: a guess, as long as nothing knows when each word is really said. The text is "text"
  // with "at" and "duration", or "clip": a voice clip (a generative clip with a "text" input), whose own time is used.
  Result<void> add_captions() {
    std::string text = op_.value("text", std::string());
    std::optional<Rational> at, duration;
    if (op_.contains("clip")) {
      ATM_TRY(auto c, clip("clip"));
      const json inputs = c.first->value("media_ref", json::object()).value("inputs", json::object());
      if (text.empty() && inputs.contains("text") && inputs["text"].is_string())
        text = inputs["text"].get<std::string>();
      const Span s = span_of(*c.first);
      at = s.in;
      duration = s.duration;
    }
    ATM_TRY(auto at_given, time("at"));
    ATM_TRY(auto duration_given, time("duration"));
    if (at_given)
      at = at_given;
    if (duration_given)
      duration = duration_given;
    if (text.empty())
      return fail("E_PARAM", "add_captions needs \"text\", or a \"clip\" with a \"text\" input.");
    if (!at || !duration || duration->num() <= 0)
      return fail("E_PARAM", "add_captions needs \"at\" and \"duration\", or a \"clip\" to take them from.");

    // The words, in sentences. A sentence ends at . ! or ? (and a long one is cut at a comma, colon or semicolon).
    struct Word {
      std::string text;
      double weight = 0.0;
    };
    std::vector<std::vector<Word>> sentences(1);
    size_t pos = 0;
    const auto is_space = [](char ch) { return ch == ' ' || ch == '\n' || ch == '\t' || ch == '\r'; };
    while (pos < text.size()) {
      while (pos < text.size() && is_space(text[pos]))
        ++pos;
      const size_t from = pos;
      while (pos < text.size() && !is_space(text[pos]))
        ++pos;
      if (pos == from)
        break;
      Word w;
      w.text = text.substr(from, pos - from);
      size_t letters = 0;
      for (const char ch : w.text)
        letters += (static_cast<unsigned char>(ch) >= 0x80 || std::isalnum(static_cast<unsigned char>(ch))) ? 1 : 0;
      w.weight = double(letters) + 1.0;
      const char last = w.text.back();
      const bool end_of_sentence = last == '.' || last == '!' || last == '?';
      w.weight += end_of_sentence ? 3.5 : (last == ',' || last == ':' || last == ';') ? 2.0 : 0.0;
      sentences.back().push_back(std::move(w));
      if (end_of_sentence || (sentences.back().size() >= 6 && (last == ',' || last == ':' || last == ';')))
        sentences.emplace_back();
    }
    if (sentences.back().empty())
      sentences.pop_back();
    double total = 0.0;
    for (const auto &sentence : sentences)
      for (const Word &w : sentence)
        total += w.weight;
    if (total <= 0.0)
      return fail("E_PARAM", "add_captions: the text has no words.");

    const std::string style = op_.value("style", std::string("pop"));
    const double size = op_.value("size", 0.07), y = op_.value("y", 0.72);
    const std::string color = op_.value("color", std::string("#ffffff")), emphasis_color = op_.value("emphasis_color", std::string("#FFE600"));
    std::vector<std::string> emphasis;
    if (op_.contains("emphasis") && op_["emphasis"].is_array())
      for (const json &e : op_["emphasis"])
        if (e.is_string()) {
          std::string w = e.get<std::string>();
          std::transform(w.begin(), w.end(), w.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
          emphasis.push_back(std::move(w));
        }
    ATM_TRY(std::string track_id, track_for("video", [&]() -> Result<std::string> {
      if (const std::string id = track_id_named("Captions"); !id.empty())
        return id;
      const std::string ph = placeholder(".track");
      add_track_op(ph, "video", "Captions", nullptr, false); // on top of the pictures
      return ph;
    }));

    const double seconds = duration->to_seconds_lossy();
    const auto rational_of = [&](double s_) { return *Rational::make(int64_t(std::llround(s_ * 1000.0)), 1000); };
    // When each word starts, in seconds from the start of the stretch: from the words of the clip's Take when the model knew them
    // (the same number of words as here), else a guess from the letters.
    std::vector<double> word_start;
    bool exact = false;
    double cumulative = 0.0;
    for (const auto &sentence : sentences)
      for (const Word &w : sentence) {
        word_start.push_back(seconds * cumulative / total);
        cumulative += w.weight;
      }
    if (op_.contains("clip") && ctx_.words_of) {
      const json known = ctx_.words_of(op_.value("clip", std::string()));
      if (known.is_array() && known.size() == word_start.size()) {
        std::vector<double> times;
        bool usable = true;
        for (const json &w : known)
          usable = usable && w.is_object() && w.contains("start") && w["start"].is_number() && (times.empty() || w["start"].get<double>() >= times.back());
        for (const json &w : known)
          times.push_back(usable ? w["start"].get<double>() : 0.0);
        if (usable) {
          for (size_t i = 0; i < times.size(); ++i)
            word_start[i] = std::min(times[i], seconds);
          exact = true;
        }
      }
    }
    if (exact)
      out_.notes.push_back("Captions are timed from the words the voice model reported.");
    size_t flat = 0;
    int index = 0;
    for (size_t si = 0; si < sentences.size(); ++si) {
      const auto &sentence = sentences[si];
      const size_t first = flat, count = sentence.size();
      const double begin = word_start[first];
      const double end = si + 1 < sentences.size() ? word_start[first + count] : seconds;
      json words = json::array();
      std::string full;
      for (const Word &w : sentence) {
        std::string lower;
        for (const char ch : w.text)
          if (static_cast<unsigned char>(ch) >= 0x80 || std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '\'')
            lower += char(std::tolower(static_cast<unsigned char>(ch)));
        json word = {{"text", w.text}, {"at", rational_of(word_start[flat] - begin).to_string()}};
        if (std::find(emphasis.begin(), emphasis.end(), lower) != emphasis.end())
          word["color"] = emphasis_color;
        words.push_back(std::move(word));
        full += (full.empty() ? "" : " ") + w.text;
        ++flat;
      }
      json content = {{"text", full}, {"size", size}, {"color", color}, {"bold", true}, {"words", std::move(words)}, {"word_pop", 0.6}};
      if (style == "pop") {
        content["outline"] = {{"color", "#000000"}, {"width", 0.1}};
        content["shadow"] = {{"color", "#000000"}, {"x", 0.06}, {"y", 0.07}, {"blur", 0.05}, {"opacity", 0.5}};
      } else if (style == "box") {
        content["background"] = {{"color", "#000000"}, {"opacity", 0.65}, {"padding", 0.3}, {"radius", 0.3}};
      }
      json value = {{"name", "Caption " + std::to_string(index + 1)},
                    {"timing", {{"record_in", plus(*at, rational_of(begin)).to_string()}, {"duration", minus(rational_of(std::max(end, begin + 0.05)), rational_of(begin)).to_string()}, {"source_in", "0"}}}, // end minus begin as rationals: neighbours meet exactly
                    {"media_ref", {{"type", "text"}}},
                    {"content", std::move(content)},
                    {"transform", {{"position", json::array({0.5, y})}, {"opacity", 1.0}}}};
      if (op_.contains("clip") && op_["clip"].is_string())
        value["caption_of"] = op_["clip"]; // the voice clip they were made from: sync_captions finds them by it
      push({{"op", "add"}, {"path", track_id + "/clips/" + placeholder(".c" + std::to_string(index))}, {"value", std::move(value)}});
      ++index;
    }
    return {};
  }

  // sync_captions: the captions made from a voice clip ({"clip": the voice}) follow its words again, after the voice was made anew (another line,
  // speed or voice): each caption's start, length and words are set from the words the Take reports. The words must be the same ones as the captions
  // have; for another text, make the captions again.
  Result<void> sync_captions() {
    ATM_TRY(auto voice, clip("clip"));
    const std::string voice_id = op_.value("clip", std::string());
    const Span vs = span_of(*voice.first);
    const json known = ctx_.words_of ? ctx_.words_of(voice_id) : json(nullptr);
    if (!known.is_array() || known.empty())
      return fail("E_NO_WORDS", "The voice clip's Take does not say when its words are said.", "Only models that know it (Kokoro) do; run the clip first.");
    struct Found {
      std::string id;
      Rational in;
      const json *node;
    };
    std::vector<Found> found;
    for (const std::string &tid : track_order())
      if (const json *t = track(tid); t && t->contains("clips"))
        for (auto it = (*t)["clips"].begin(); it != (*t)["clips"].end(); ++it)
          if (it->value("caption_of", std::string()) == voice_id)
            found.push_back({it.key(), span_of(*it).in, &*it});
    if (found.empty())
      return fail("E_NO_CAPTIONS", "No captions were made from this clip.", "Make them with add_captions.");
    std::sort(found.begin(), found.end(), [](const Found &a, const Found &b) { return compare(a.in, b.in) < 0; });
    size_t have = 0;
    for (const Found &f : found)
      have += f.node->value("content", json::object()).value("words", json::array()).size();
    if (have != known.size())
      return fail("E_WORDS_DIFFER", "The captions have " + std::to_string(have) + " words and the voice now says " + std::to_string(known.size()) + ".",
                  "The text changed: make the captions again.");
    const double seconds = vs.duration.to_seconds_lossy();
    const auto rational_of = [&](double s_) { return *Rational::make(int64_t(std::llround(s_ * 1000.0)), 1000); };
    size_t flat = 0;
    for (size_t k = 0; k < found.size(); ++k) {
      json words = found[k].node->value("content", json::object()).value("words", json::array());
      const double begin = std::min(known[flat].value("start", 0.0), seconds);
      for (json &w : words) {
        w["at"] = rational_of(std::max(0.0, std::min(known[flat].value("start", 0.0), seconds) - begin)).to_string();
        ++flat;
      }
      const double end = k + 1 < found.size() ? std::min(known[flat].value("start", seconds), seconds) : seconds;
      push({{"op", "replace"}, {"path", found[k].id + "/timing/record_in"}, {"value", plus(vs.in, rational_of(begin)).to_string()}});
      push({{"op", "replace"}, {"path", found[k].id + "/timing/duration"}, {"value", minus(rational_of(std::max(end, begin + 0.05)), rational_of(begin)).to_string()}});
      push({{"op", "replace"}, {"path", found[k].id + "/content/words"}, {"value", std::move(words)}});
    }
    out_.notes.push_back("Re-timed " + std::to_string(found.size()) + " captions from the voice's words.");
    return {};
  }

  Result<void> add_adjustment() {
    ATM_TRY(std::string track_id, track_for("video", [&]() -> Result<std::string> {
      if (const std::string id = track_id_named("Effects"); !id.empty())
        return id;
      const std::string ph = placeholder(".track");
      const std::string titles = track_id_named("Titles"); // under the titles, so text stays sharp
      add_track_op(ph, "video", "Effects", titles.empty() ? json(nullptr) : json{{"before", titles}}, true); // locked to the cut
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

  // The two clips of a cut and how far a transition over it reaches before and after the cut.
  struct Cut {
    std::string a, b;
    const doc::NodeRef *ra = nullptr, *rb = nullptr;
    Span sa, sb;
    Rational in, out;
  };

  // Reads "between" ([first clip, second clip], touching on one track), "duration" (1 s) and "alignment" (center, start
  // or end: where the transition sits on the cut) of an op that works on a cut.
  Result<Cut> cut_spec(const char *op_name) {
    const auto between = op_.find("between");
    if (between == op_.end() || !between->is_array() || between->size() != 2)
      return fail("E_PARAM", std::string(op_name) + " needs \"between\": [first clip, second clip].");
    Cut c;
    c.a = (*between)[0].is_string() ? (*between)[0].get<std::string>() : "";
    c.b = (*between)[1].is_string() ? (*between)[1].get<std::string>() : "";
    c.ra = doc_.find(c.a);
    c.rb = doc_.find(c.b);
    if (!c.ra || !c.rb || id_prefix(c.a) != "clp" || id_prefix(c.b) != "clp")
      return fail("E_UNKNOWN_CLIP", "\"between\" must name two clips.", "Use clip IDs or $new: names of clips made earlier in this call.");
    if (c.ra->parent != c.rb->parent)
      return fail("TRANSITION_NOT_ADJACENT", "The two clips are on different tracks.", "A transition joins two clips on one track.");
    c.sa = span_of(*c.ra->node);
    c.sb = span_of(*c.rb->node);
    if (compare(c.sa.end(), c.sb.in) != 0)
      return fail("TRANSITION_NOT_ADJACENT",
                  "The first clip ends at " + seconds_text(c.sa.end()) + " s but the second starts at " + seconds_text(c.sb.in) + " s.",
                  "Put the second clip at the first one's end (append, or \"at\": \"" + c.sa.end().to_string() + "\").");
    ATM_TRY(Rational d, time_or("duration", Rational::from_int(1)));
    const std::string alignment = op_.value("alignment", std::string("center"));
    if (alignment == "center") {
      c.in = *Rational::make(d.num(), d.den() * 2);
      c.out = minus(d, c.in);
    } else if (alignment == "start") { // begins at the cut
      c.out = d;
    } else if (alignment == "end") { // ends at the cut
      c.in = d;
    } else {
      return fail("E_PARAM", "\"alignment\" must be center, start or end.");
    }
    return c;
  }

  // The media a clip has before its in point and after its out point; nullopt: no limit (text, stills) or not known.
  // The rule of the validator (atm_patch handles_of).
  struct Room {
    std::optional<Rational> before, after;
  };
  static Room room_of(const json &clip, const Span &s) {
    Room r;
    const json ref = clip.value("media_ref", json::object());
    if (const std::string type = ref.value("type", ""); type == "text" || type == "image")
      return r;
    r.before = s.source_in;
    if (const auto total = Rational::parse(ref.value("duration", std::string())); total && total->num() > 0)
      r.after = minus(*total, plus(s.source_in, s.duration));
    return r;
  }

  // A track locked to the cut ("sync_lock": true) follows the edits that take time out of another track.
  bool is_locked(const std::string &track_id) const {
    const json *t = track(track_id);
    return t && t->contains("sync_lock") && (*t)["sync_lock"].is_boolean() && (*t)["sync_lock"].get<bool>();
  }

  // The tracks besides the edited one whose clips ripple with it: the op's "ripple" is "synced" (the default: the tracks
  // locked to the cut), "track" (none), "all", or a list of track IDs.
  Result<std::vector<std::string>> ripple_tracks(const std::string &own) const {
    std::vector<std::string> out;
    const auto it = op_.find("ripple");
    const std::string mode = it == op_.end() || it->is_null() ? "synced" : it->is_string() ? it->get<std::string>() : std::string();
    if (mode == "track")
      return out;
    if (mode == "all" || mode == "synced") {
      for (const std::string &id : track_order())
        if (id != own && (mode == "all" || is_locked(id)))
          out.push_back(id);
      return out;
    }
    if (it == op_.end() || !it->is_array())
      return fail("E_PARAM", "\"ripple\" must be \"synced\", \"track\", \"all\" or a list of track IDs.");
    for (const json &v : *it) {
      const std::string id = v.is_string() ? v.get<std::string>() : std::string();
      if (!track(id))
        return fail("E_UNKNOWN_TRACK", "There is no track \"" + id + "\" to ripple.",
                    "Use track IDs from project.inspect, \"synced\", \"all\", or leave \"ripple\" out.");
      if (id != own && std::find(out.begin(), out.end(), id) == out.end())
        out.push_back(id);
    }
    return out;
  }

  std::vector<std::string> done_; // clips this op has already given a new span or removed

  bool seen(const std::string &id) const { return std::find(done_.begin(), done_.end(), id) != done_.end(); }

  // Gives a clip a new span, checked against its media; the clip counts as done. `why` explains an empty result.
  Result<void> change(const std::string &id, Span after, const std::string &why = {}) {
    if (after.duration.num() <= 0)
      return fail("E_MEDIA_RANGE", "Clip " + id + " would be empty" + (why.empty() ? "." : ": " + why + "."),
                  "Use a shorter transition, or clips with more media beyond the cut.");
    ATM_CHECK(check_media(node_of(id), id, after));
    put_span(id, span_of(node_of(id)), after);
    done_.push_back(id);
    return {};
  }

  struct SpanRipple {
    int moved = 0, cut = 0, removed = 0;
    std::vector<std::string> left_alone;
  };

  // The clips of `tracks` after time was taken out of [start, end) of another track (shift = end - start), as in a ripple
  // delete of that span: before it, nothing; after it, moved up; ending inside it, cut short; starting inside it, the head
  // is cut and the keyframes count from the new start; wholly inside it, removed; spanning it, shortened by the span when
  // the clip has no media of its own (a title, an effect layer, a still), and left alone when it does (it would need a
  // split). A transition on a clip whose length changes is dropped. Clips this op has already handled are skipped.
  Result<SpanRipple> ripple_span(const std::vector<std::string> &tracks, const Rational &start, const Rational &end,
                                 const Rational &shift) {
    SpanRipple r;
    for (const std::string &tid : tracks) {
      const json *t = track(tid);
      if (!t || !t->contains("clips"))
        continue;
      for (auto it = (*t)["clips"].begin(); it != (*t)["clips"].end(); ++it) {
        const std::string id = it.key();
        if (seen(id))
          continue;
        const Span sp = span_of(*it);
        const Rational clip_end = sp.end();
        if (compare(clip_end, start) <= 0)
          continue; // before the span
        if (compare(sp.in, end) >= 0) { // after it: up by the span
          ATM_CHECK(change(id, {minus(sp.in, shift), sp.duration, sp.source_in}));
          ++r.moved;
          continue;
        }
        const bool starts_before = compare(sp.in, start) < 0, ends_after = compare(clip_end, end) > 0;
        const bool media = it->value("media_ref", json::object()).value("type", "") == "file";
        if (starts_before && ends_after && media) { // would need a split: left as it is
          r.left_alone.push_back(it->value("name", id));
          continue;
        }
        drop_transitions(id, tid); // its length changes
        if (!starts_before && !ends_after) { // wholly inside the span
          push({{"op", "remove"}, {"path", id}});
          done_.push_back(id);
          ++r.removed;
        } else if (starts_before && ends_after) { // spans it
          ATM_CHECK(change(id, {sp.in, minus(sp.duration, shift), sp.source_in}));
          ++r.cut;
        } else if (starts_before) { // ends inside it
          ATM_CHECK(change(id, {sp.in, minus(start, sp.in), sp.source_in}));
          ++r.cut;
        } else { // starts inside it: the part in the span is gone, the rest starts where the span began
          const Rational lost = minus(end, sp.in);
          ATM_CHECK(change(id, {start, minus(sp.duration, lost), plus(sp.source_in, lost)}));
          shift_key_times(id, lost);
          ++r.cut;
        }
      }
    }
    return r;
  }

  void ripple_notes(const SpanRipple &r, const Rational &shift, bool any_tracks) {
    if (any_tracks) {
      std::string more = "On the other tracks that follow (locked to the cut, or named by \"ripple\"): moved " + std::to_string(r.moved) +
                         " clip" + (r.moved == 1 ? "" : "s") + " " + seconds_text(shift) + " s earlier, shortened " + std::to_string(r.cut);
      if (r.removed > 0)
        more += ", removed " + std::to_string(r.removed) + " that lay wholly inside the removed span";
      out_.notes.push_back(more + ".");
    }
    if (!r.left_alone.empty()) {
      std::string names;
      for (const std::string &n : r.left_alone)
        names += (names.empty() ? "" : ", ") + n;
      out_.notes.push_back("Left in place, because it carries media and spans the cut: " + names +
                           ". Split it at the cut if it should follow.");
    }
  }

  // A clip that lost `delta` from its head: its keyframes (the transform's and its effects') count from the new start.
  void shift_key_times(const std::string &id, const Rational &delta) {
    const json &n = node_of(id);
    const auto shift_props = [&](const json &props) {
      if (!props.is_object())
        return;
      for (const auto &[prop, keys] : props.items())
        for (auto k = keys.begin(); k != keys.end(); ++k)
          if (const auto t = Rational::parse(k->value("t", std::string("0"))))
            push({{"op", "replace"}, {"path", k.key() + "/t"}, {"value", minus(*t, delta).to_string()}});
    };
    if (n.contains("transform") && n["transform"].contains("keyframes"))
      shift_props(n["transform"]["keyframes"]);
    if (n.contains("effects") && n["effects"].is_object())
      for (const auto &[fx_id, fx] : n["effects"].items())
        if (fx.contains("keyframes"))
          shift_props(fx["keyframes"]);
  }

  // Makes room for a transition over a cut. Where the media beyond the cut is short, the clip on that side is trimmed
  // by what is missing (the end of the first clip, the start of the second), so the transition can use the media that
  // used to be on screen. The second clip then moves up to meet the first, and every later clip of its track follows by
  // the same amount, so no gap opens; linked clips (the sound of a video) are cut and moved with their partners.
  //
  // The span taken out of the track is [cut - short_a, cut + short_b). The tracks locked to the cut (sync_lock), or named
  // by the op's "ripple" ("all", "track" for none, or a list of track IDs), follow it as ripple_span describes.
  Result<void> make_room_at(const Cut &cut) {
    const auto pairs = linked_pairs(cut.a, cut.b); // read before any change
    std::vector<std::string> side_a{cut.a}, side_b{cut.b};
    for (const auto &[la, lb] : pairs) {
      side_a.push_back(la);
      side_b.push_back(lb);
    }
    Rational short_a, short_b; // what is missing after the first clip's out point, and before the second's in point
    for (const std::string &id : side_a)
      if (const Room r = room_of(node_of(id), span_of(node_of(id))); r.after && compare(cut.out, *r.after) > 0)
        if (const Rational missing = minus(cut.out, *r.after); compare(missing, short_a) > 0)
          short_a = missing;
    for (const std::string &id : side_b)
      if (const Room r = room_of(node_of(id), span_of(node_of(id))); r.before && compare(cut.in, *r.before) > 0)
        if (const Rational missing = minus(cut.in, *r.before); compare(missing, short_b) > 0)
          short_b = missing;
    if (short_a.num() == 0 && short_b.num() == 0) {
      out_.notes.push_back("There is already enough media on both sides of the cut: nothing was trimmed.");
      return {};
    }
    const Rational shift = plus(short_a, short_b); // how much shorter the track gets after the cut
    const std::string why = "making room takes " + seconds_text(short_a) + " s off the end of the first clip and " +
                            seconds_text(short_b) + " s off the start of the second";
    for (const std::string &id : side_a) { // the end of the first clip comes in
      const Span s = span_of(node_of(id));
      ATM_CHECK(change(id, {s.in, minus(s.duration, short_a), s.source_in}, why));
    }
    for (const std::string &id : side_b) { // the start of the second comes in, and it meets the first clip's new end
      const Span s = span_of(node_of(id));
      ATM_CHECK(change(id, {minus(s.in, short_a), minus(s.duration, short_b), plus(s.source_in, short_b)}, why));
    }
    int later = 0; // every later clip on the track, and the clips linked to them, moves up by the same amount
    const auto follow = [&](const std::string &id) -> Result<void> {
      if (seen(id))
        return {};
      const Span s = span_of(node_of(id));
      ATM_CHECK(change(id, {minus(s.in, shift), s.duration, s.source_in}, why));
      ++later;
      return {};
    };
    std::vector<std::string> following;
    if (const json *t = track(cut.ra->parent); t && t->contains("clips"))
      for (auto it = (*t)["clips"].begin(); it != (*t)["clips"].end(); ++it)
        if (!seen(it.key()) && compare(span_of(*it).in, cut.sb.in) > 0)
          following.push_back(it.key());
    for (const std::string &id : following) {
      ATM_CHECK(follow(id));
      for (const Member &m : linked(id))
        ATM_CHECK(follow(m.id));
    }
    ATM_TRY(std::vector<std::string> others, ripple_tracks(cut.ra->parent));
    ATM_TRY(SpanRipple spans, ripple_span(others, minus(cut.sb.in, short_a), plus(cut.sb.in, short_b), shift));
    // A transition that already joins these clips at this cut no longer fits: it goes, with a note.
    const auto drop_between = [&](const std::string &from, const std::string &to) {
      const doc::NodeRef *r = doc_.find(from);
      const json *t = r ? track(r->parent) : nullptr;
      if (t && t->contains("transitions"))
        for (auto it = (*t)["transitions"].begin(); it != (*t)["transitions"].end(); ++it)
          if (it->value("from", "") == from && it->value("to", "") == to) {
            push({{"op", "remove"}, {"path", it.key()}});
            out_.notes.push_back("Removed transition " + it.key() + ": its cut moved. Add it again with add_transition.");
          }
    };
    drop_between(cut.a, cut.b);
    for (const auto &[la, lb] : pairs)
      drop_between(la, lb);
    std::string note = "Made room for the transition:";
    if (short_a.num() > 0)
      note += " trimmed " + seconds_text(short_a) + " s from the end of the first clip";
    if (short_b.num() > 0)
      note += std::string(short_a.num() > 0 ? " and " : " ") + "trimmed " + seconds_text(short_b) + " s from the start of the second";
    note += ", and moved " + std::to_string(later) + " later clip" + (later == 1 ? "" : "s") + " " + seconds_text(shift) +
            " s earlier. The track is " + seconds_text(shift) + " s shorter after the cut.";
    out_.notes.push_back(note);
    ripple_notes(spans, shift, !others.empty());
    return {};
  }

  // make_room {between: [first, second], duration?, alignment?}: the room a transition of that length would need.
  Result<void> make_room() {
    ATM_TRY(Cut cut, cut_spec("make_room"));
    return make_room_at(cut);
  }

  Result<void> add_transition() {
    ATM_TRY(Cut cut, cut_spec("add_transition"));
    const std::string &a = cut.a, &b = cut.b;
    const std::string type = op_.value("type", std::string("attome.dissolve"));
    const std::string kind = eval::transition_id(type);
    if (kind.empty())
      return fail("TRANSITION_UNSUPPORTED", "The transition \"" + type + "\" is not available.",
                  "Use one of: " + eval::transition_ids() + ".");
    json params = json::object(); // a wipe or push: the side the incoming clip enters from (a wipe: and its softness)
    if (kind == "wipe" || kind == "push" || kind == "slide") {
      const std::string direction = op_.value("direction", std::string("left"));
      eval::WipeDirection dir;
      if (!eval::parse_wipe_direction(direction, dir))
        return fail("E_PARAM", "\"direction\" must be left, right, up or down.");
      params = {{"direction", direction}};
      if (kind == "wipe")
        params["softness"] = op_.value("softness", 0.1);
    }
    if (kind == "iris") {
      const double softness = op_.value("softness", 0.15);
      if (softness < 0.01 || softness > 1.0)
        return fail("E_PARAM", "\"softness\" must be from 0.01 to 1.", "The width of the iris's soft edge as a fraction of the picture; 0.15 is a good start.");
      params = {{"softness", softness}};
    }
    if (kind == "zoom") {
      const double amount = op_.value("amount", eval::kZoomDefault);
      if (amount < eval::kZoomMin || amount > eval::kZoomMax)
        return fail("E_PARAM", "\"amount\" must be from " + std::to_string(eval::kZoomMin) + " to " + std::to_string(eval::kZoomMax) + ".",
                    "0.5 is half as big again, 1 doubles the picture.");
      const std::string direction = op_.value("direction", std::string("in"));
      eval::ZoomDirection zdir;
      if (!eval::parse_zoom_direction(direction, zdir))
        return fail("E_PARAM", "A zoom's \"direction\" must be in or out.", "in: the old picture grows; out: it shrinks away.");
      params = {{"amount", amount}, {"direction", direction}};
    }
    if (op_.value("make_room", false)) // trim and move up what the transition needs, instead of being refused
      ATM_CHECK(make_room_at(cut));
    push({{"op", "add"},
          {"path", cut.ra->parent + "/transitions/" + placeholder()},
          {"value", transition_value(kind, a, b, cut.in, cut.out, params)}});
    int n = 0; // the linked sound clips that meet at the same cut cross-fade over the same range
    for (const auto &[la, lb] : linked_pairs(a, b))
      push({{"op", "add"},
            {"path", doc_.find(la)->parent + "/transitions/" + placeholder(".audio" + std::to_string(n++))},
            {"value", {{"type", "attome.dissolve"}, {"from", la}, {"to", lb}, {"in_offset", cut.in.to_string()}, {"out_offset", cut.out.to_string()}}}});
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
    if (ripple) { // the tracks locked to the cut (or named by "ripple") follow as for a trimmed span
      ATM_TRY(std::vector<std::string> others, ripple_tracks(c.second));
      std::erase_if(others, [&](const std::string &tid) {
        return std::any_of(all.begin(), all.end(), [&](const Member &m) { return m.track == tid; }); // moved above
      });
      if (!others.empty()) {
        const Span s = span_of(*c.first);
        for (const Member &m : all)
          done_.push_back(m.id);
        ATM_TRY(SpanRipple spans, ripple_span(others, s.in, s.end(), s.duration));
        ripple_notes(spans, s.duration, true);
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

  // Gives every collection member below `node` an ID of its own, named from `ph`: keyframes, effects (and their keyframes). A
  // generative clip's workflow gets a fresh copy of its nodes and links, and no Takes: the copy makes its own, from the same inputs.
  void fresh_children(json &node, const std::string &ph) {
    int n = 0;
    if (node.contains("transform") && node["transform"].contains("keyframes") && node["transform"]["keyframes"].is_object())
      for (auto &[prop, keys] : node["transform"]["keyframes"].items()) {
        json moved = json::object();
        for (const auto &[kid, key] : keys.items())
          moved[ph + ".k" + std::to_string(n++)] = key;
        keys = std::move(moved);
      }
    if (node.contains("effects") && node["effects"].is_object()) {
      json fx = json::object();
      for (const auto &[fid, e] : node["effects"].items()) {
        json one = e;
        if (one.contains("keyframes") && one["keyframes"].is_object())
          for (auto &[param, keys] : one["keyframes"].items()) {
            json moved = json::object();
            for (const auto &[kid, key] : keys.items())
              moved[ph + ".k" + std::to_string(n++)] = key;
            keys = std::move(moved);
          }
        fx[ph + ".fx" + std::to_string(n++)] = std::move(one);
      }
      node["effects"] = std::move(fx);
      node.erase("effect_order");
    }
    if (node.contains("media_ref") && node["media_ref"].is_object() && node["media_ref"].value("type", std::string()) == "workflow") {
      json &ref = node["media_ref"];
      if (ref.contains("workflow") && ref["workflow"].is_object())
        ref["workflow"] = gen::fresh_copy(ref["workflow"], ref["workflow"].value("source", std::string()));
      for (const char *gone : {"takes", "take_order", "selected", "locked"})
        ref.erase(gone);
    }
  }

  // duplicate: copies of clips (with their effects, keyframes, fades, and a generative clip's own workflow), each at a time
  // and, optionally, on another track of the same kind: {"clips": [{"clip": ID, "at": time, "track": ID?}, ...]}. An item may
  // carry {"snapshot": <a clip's object>, "track": ID} instead of "clip": paste. Clips that
  // were linked stay linked to each other in the copy. The copies are named "$new:<id>.c0", ".c1" ... in the order given.
  Result<void> duplicate() {
    const json list = op_.value("clips", json::array());
    if (!list.is_array() || list.empty())
      return fail("E_PARAM", "duplicate needs \"clips\": [{\"clip\": ID, \"at\": time}, ...].");
    std::map<std::string, std::string> groups; // the old link group -> the group of the copies
    int i = 0;
    for (const json &item : list) {
      // A clip of the sequence by its ID, or a snapshot of one (what a Copy took, which may be gone from the document by now)
      // with the track it is put on.
      const bool from_snapshot = item.is_object() && item.contains("snapshot") && item["snapshot"].is_object();
      const std::string id = item.is_object() ? item.value("clip", std::string()) : std::string();
      const doc::NodeRef *ref = from_snapshot || id.empty() ? nullptr : doc_.find(id);
      if (!from_snapshot && (!ref || id_prefix(id) != "clp" || !track(ref->parent)))
        return fail("E_UNKNOWN_CLIP", "clips[" + std::to_string(i) + "].clip must be the ID of a clip of this sequence, not \"" + id + "\".");
      std::string track_id = from_snapshot ? std::string() : ref->parent;
      if (item.contains("track") && item["track"].is_string())
        track_id = item["track"].get<std::string>();
      const json *target = track(track_id);
      if (!target)
        return fail("E_UNKNOWN_TRACK", "There is no track \"" + track_id + "\" in this sequence.",
                    from_snapshot ? "A snapshot needs \"track\"." : "");
      if (!from_snapshot && target->value("kind", "video") != track(ref->parent)->value("kind", "video"))
        return fail("E_TRACK_KIND", "A copy of a clip stays on a track of its own kind.");
      if (!item.contains("at"))
        return fail("E_PARAM", "clips[" + std::to_string(i) + "] needs \"at\".");
      auto at = parse_time(item["at"], TimeContext{ctx_.rate});
      if (!at)
        return fail("E_PARAM", "clips[" + std::to_string(i) + "].at is not a time: " + at.error().message);
      json copy = from_snapshot ? item["snapshot"] : *ref->node;
      copy["timing"]["record_in"] = at->to_string();
      if (copy.contains("link_group") && copy["link_group"].is_string()) {
        auto [found, fresh] = groups.emplace(copy["link_group"].get<std::string>(), std::string());
        if (fresh)
          found->second = new_id("lnk");
        copy["link_group"] = found->second;
      }
      const std::string ph = placeholder(".c" + std::to_string(i));
      fresh_children(copy, ph);
      push({{"op", "add"}, {"path", track_id + "/clips/" + ph}, {"value", std::move(copy)}});
      ++i;
    }
    return {};
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
    if (def->clip_only && ref->node->value("media_ref", json::object()).value("type", std::string()) == "adjustment")
      return fail("EFFECT_UNSUPPORTED", std::string("The ") + def->title + " effect works on a clip's own picture, not on an adjustment layer.",
                  "Add it to the clip that has the picture.");
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
