// timeline.edit: the engine side of the edit ops (timeline.cpp turns each op into patch ops).
#include "engine_impl.hpp"

namespace atm::api {

Result<json> Engine::Impl::time_parse(const json &params) {
  const auto value = params.find("value");
  if (value == params.end())
    return bad_param("value", "is required");
  TimeContext ctx;
  if (const auto rate = params.find("rate"); rate != params.end() && rate->is_string()) {
    ATM_TRY(Rational r, Rational::parse(rate->get_ref<const std::string &>()));
    ctx.rate = r;
  }
  ATM_TRY(RationalTime t, parse_time(*value, ctx));
  return to_json(format_time(t, ctx.rate));
}

  // Replaces every string equal to a placeholder of an earlier op with the ID it became.
void Engine::Impl::resolve_placeholders(json &v, const json &id_map) {
  if (v.is_string()) {
    if (const auto it = id_map.find(v.get_ref<const std::string &>()); it != id_map.end())
      v = *it;
  } else if (v.is_structured()) {
    for (json &e : v)
      resolve_placeholders(e, id_map);
  }
}

  // Each op becomes patch ops against a scratch copy holding the earlier ops, so later ops see what earlier ones made;
  // the whole call is then applied to the project as one Patch (validated, journaled, one undo step per task).
Result<json> Engine::Impl::timeline_edit(const json &params) {
  ATM_TRY(Project *pr, project(params));
  const auto ops = params.find("ops");
  if (ops == params.end() || !ops->is_array() || ops->empty())
    return bad_param("ops", "is required: a list of timeline ops (guide.get topic \"timeline\")");
  const json &root = pr->doc.root();
  timeline::Context ctx;
  ATM_TRY(const SequenceRef sq, sequence_of(root, params));
  ctx.sequence = sq.id;
  ATM_TRY(Rational rate, Rational::parse(sq.node->value("rate", std::string("30"))));
  ctx.rate = rate;
  ctx.project_dir = to_utf8(pr->dir);
  ctx.probe = [this](const std::string &path) { return media_probe({{"path", path}}); };
  ctx.still = [pr, seq = ctx.sequence](const std::string &clip_id, double at) -> Result<json> {
    ATM_TRY(render::Composition comp, render::compile(pr->doc.root(), seq, to_utf8(pr->dir)));
    const auto it = std::find_if(comp.layers.begin(), comp.layers.end(), [&](const render::Layer &l) { return l.clip_id == clip_id; });
    if (it == comp.layers.end() || it->path.empty())
      return fail(ErrorCode::InvalidArgument, "E_PARAM", "Clip " + clip_id + " has no picture to freeze.");
    render::Layer l = *it; // its own picture only: the frozen frame takes the clip's place and the clip's transform
    l.xf = render::Transform{};
    l.effects.clear();
    l.opacity = 1.0f;
    l.opacity_keys = l.position_keys = l.scale_keys = l.rotation_keys = l.anchor_keys = eval::Curve{};
    l.mix_with = l.mixed_by = -1;
    l.mix_frames = 0;
    const json &node = *pr->doc.find(clip_id)->node;
    const json ref = node.value("media_ref", json::object());
    const int w = std::max(16, ref.value("width", comp.width)) & ~1, h = std::max(16, ref.value("height", comp.height)) & ~1;
    comp.width = w;
    comp.height = h;
    comp.layers = {l};
    const int64_t frame = std::clamp<int64_t>(int64_t(std::floor(at * double(comp.rate_num) / double(comp.rate_den) + 1e-6)), l.start_frame,
                                              l.start_frame + std::max<int64_t>(1, l.frames) - 1);
    render::Renderer renderer(std::move(comp), w, h);
    std::vector<uint8_t> nv12(media::nv12_size(renderer.width(), renderer.height()));
    ATM_CHECK(renderer.render(frame, nv12.data()));
    std::vector<uint8_t> bgrx(size_t(renderer.width()) * size_t(renderer.height()) * 4);
    media::nv12_to_bgrx(nv12.data(), renderer.width(), renderer.height(), bgrx.data());
    const fs::path dir = pr->dir / ".attome" / "freeze";
    ATM_CHECK(storage::make_dirs(dir));
    const fs::path file = dir / (clip_id + "_" + std::to_string(frame) + ".jpg");
    ATM_CHECK(media::write_jpeg(to_utf8(file), bgrx.data(), renderer.width() & ~1, renderer.height() & ~1, 0.95f));
    return json{{"path", to_utf8(file)}, {"width", renderer.width() & ~1}, {"height", renderer.height() & ~1}};
  };
  ctx.words_of = [pr](const std::string &clip_id) -> json {
    const doc::NodeRef *ref = pr->doc.find(clip_id);
    if (!ref)
      return nullptr;
    const json &media = (*ref->node).contains("media_ref") ? (*ref->node)["media_ref"] : json::object();
    const std::string selected = media.value("selected", std::string());
    if (selected.empty() || !media.contains("takes") || !media["takes"].contains(selected))
      return nullptr;
    const json &outputs = media["takes"][selected].value("outputs", json::object());
    if (!outputs.contains("words") || !outputs["words"].is_object())
      return nullptr;
    fs::path file = to_path(outputs["words"].value("path", std::string()));
    if (file.is_relative())
      file = pr->dir / file;
    const auto text = storage::read_file(file);
    if (!text)
      return nullptr;
    const json list = json::parse(*text, nullptr, false);
    return list.is_array() ? list : json(nullptr);
  };

  ATM_TRY(doc::Document scratch, doc::Document::from_json(root));
  json all = json::array(), id_map = json::object(), notes = json::array();
  for (size_t i = 0; i < ops->size(); ++i) {
    json op = (*ops)[i];
    resolve_placeholders(op, id_map);
    if (op.is_object() && op.contains("id")) // its own name stays a placeholder
      op["id"] = (*ops)[i]["id"];
    ATM_TRY(timeline::Built built, timeline::build(scratch, op, i, ctx));
    auto applied = patch::apply(scratch, built.ops, {.keep = true, .validate = false});
    if (!applied) {
      Error e = std::move(applied.error());
      e.message = "ops[" + std::to_string(i) + "] (" + op.value("op", std::string("?")) + "): " + e.message;
      e.details = {{"op_index", i}};
      return tl::unexpected(std::move(e));
    }
    for (auto it = built.names.begin(); it != built.names.end(); ++it)
      if (const auto made = applied->id_map.find(it.key()); made != applied->id_map.end())
        id_map[it.key()] = *made;
    for (json &o : applied->ops)
      all.push_back(std::move(o));
    for (json &n : built.notes)
      notes.push_back(std::move(n));
  }
  // The History says what was done: the first edit by name ("Add clip"), and how many more came with it.
  std::string made_label = "Timeline edit";
  if (!ops->empty() && ops->front().is_object() && ops->front().value("op", std::string()).size() > 0) {
    made_label = ops->front().value("op", std::string());
    std::replace(made_label.begin(), made_label.end(), '_', ' ');
    made_label[0] = char(std::toupper(static_cast<unsigned char>(made_label[0])));
    if (ops->size() > 1)
      made_label += " and " + std::to_string(ops->size() - 1) + " more";
  }
  json p = {{"project", params["project"]},
            {"patch", {{"ops", std::move(all)}, {"label", params.value("label", made_label)}}},
            {"dry_run", params.value("dry_run", false)}};
  if (params.contains("task_id"))
    p["task_id"] = params["task_id"];
  ATM_TRY(json res, project_patch(p));
  res["id_map"] = std::move(id_map);
  res["notes"] = std::move(notes);
  res["duration"] = timeline::sequence_duration(scratch, ctx);
  if (!res.value("applied", false) && res.contains("inverse")) // a dry run: the inverse is noise for an agent
    res.erase("inverse");
  return res;
}

} // namespace atm::api
