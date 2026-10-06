#include "atm/gen/models.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>

namespace atm::gen {
namespace {

struct Registry {
  std::mutex mutex;
  std::map<std::string, std::unique_ptr<ModelDecl>, std::less<>> models;
};
Registry &registry() {
  static Registry r;
  return r;
}

std::string_view short_kind(std::string_view name) {
  constexpr std::string_view ns = "attome.";
  return name.substr(0, ns.size()) == ns ? name.substr(ns.size()) : name;
}

tl::unexpected<Error> bad(const std::string &id, std::string path, std::string message, std::string hint) {
  Error e;
  e.code = ErrorCode::SchemaViolation;
  e.rule = "M_DECL";
  e.path = std::move(path);
  e.message = "The declaration of model \"" + id + "\" " + std::move(message);
  e.hint = std::move(hint);
  return tl::unexpected(std::move(e));
}

std::string number_text(double v) {
  char text[32];
  if (v == std::floor(v) && std::fabs(v) < 1e15)
    std::snprintf(text, sizeof text, "%lld", static_cast<long long>(v));
  else
    std::snprintf(text, sizeof text, "%g", v);
  return text;
}

std::string joined(const std::vector<std::string> &items) {
  std::string out;
  for (const std::string &s : items)
    out += (out.empty() ? "" : ", ") + s;
  return out;
}

} // namespace

bool ModelDecl::does(std::string_view kind) const {
  return std::find(kinds.begin(), kinds.end(), short_kind(kind)) != kinds.end();
}

bool ModelDecl::takes(std::string_view input) const { return std::find(accepts.begin(), accepts.end(), input) != accepts.end(); }

const SettingDecl *ModelDecl::setting(std::string_view name) const {
  for (const SettingDecl &s : settings)
    if (s.name == name)
      return &s;
  return nullptr;
}

std::string ModelDecl::setting_names() const {
  std::string out;
  for (const SettingDecl &s : settings)
    out += (out.empty() ? "" : ", ") + s.name;
  return out.empty() ? "none" : out;
}

Result<ModelDecl> parse_model(const json &d) {
  ModelDecl m;
  if (!d.is_object() || !d.contains("id") || !d["id"].is_string() || d["id"].get_ref<const std::string &>().empty())
    return bad("?", "id", "has no \"id\".", "Give it the model's catalog ID.");
  m.id = d["id"].get<std::string>();
  if (d.contains("title") && d["title"].is_string())
    m.title = d["title"].get<std::string>();
  const auto strings = [&](const char *key, std::vector<std::string> &out) -> bool {
    const auto it = d.find(key);
    if (it == d.end())
      return true;
    if (!it->is_array())
      return false;
    for (const json &e : *it) {
      if (!e.is_string())
        return false;
      out.push_back(e.get<std::string>());
    }
    return true;
  };
  if (!strings("kinds", m.kinds) || m.kinds.empty())
    return bad(m.id, "kinds", "needs \"kinds\": the node kinds it does.", "Use some of: " + kind_ids() + ".");
  for (std::string &k : m.kinds) {
    k = std::string(short_kind(k));
    if (!find_kind(k))
      return bad(m.id, "kinds", "names the kind \"" + k + "\", which does not exist.", "Use some of: " + kind_ids() + ".");
  }
  if (!strings("accepts", m.accepts))
    return bad(m.id, "accepts", "has an \"accepts\" that is not a list of input names.", "Example: [\"start_image\", \"end_image\"].");
  if (const auto s = d.find("seconds"); s != d.end() && s->is_object()) {
    m.seconds_min = s->value("min", 0.0);
    m.seconds_max = s->value("max", 0.0);
    if (m.seconds_max != 0.0 && m.seconds_min > m.seconds_max)
      return bad(m.id, "seconds", "has a shortest length above its longest.", "Swap \"min\" and \"max\".");
  }
  if (const auto s = d.find("sizes"); s != d.end() && s->is_object()) {
    m.size_multiple = std::max(1, s->value("multiple", 1));
    m.max_pixels = s->value("max_pixels", int64_t(0));
  }
  if (const auto f = d.find("needs_files"); f != d.end() && f->is_boolean())
    m.needs_files = f->get<bool>();
  m.note = d.value("note", std::string());
  if (!strings("voices", m.voices))
    return bad(m.id, "voices", "has \"voices\" that is not a list of names.", "Example: [\"am_michael\", \"af_heart\"].");
  m.default_voice = d.value("default_voice", m.voices.empty() ? std::string() : m.voices.front());
  if (const auto all = d.find("settings"); all != d.end()) {
    if (!all->is_object())
      return bad(m.id, "settings", "has \"settings\" that is not an object.", "Write {\"<name>\": {\"type\": …}}.");
    for (auto it = all->begin(); it != all->end(); ++it) {
      SettingDecl s;
      s.name = it.key();
      const std::string path = "settings/" + s.name;
      const std::string type = it->is_object() ? it->value("type", std::string()) : std::string();
      if (type == "integer")
        s.type = SettingDecl::Type::integer;
      else if (type == "number")
        s.type = SettingDecl::Type::number;
      else if (type == "boolean")
        s.type = SettingDecl::Type::boolean;
      else if (type == "choice")
        s.type = SettingDecl::Type::choice;
      else if (type == "text")
        s.type = SettingDecl::Type::text;
      else
        return bad(m.id, path + "/type", "has the setting \"" + s.name + "\" of type \"" + type + "\".",
                   "Use integer, number, boolean, choice or text.");
      if (s.type == SettingDecl::Type::integer || s.type == SettingDecl::Type::number) {
        if (!it->contains("min") || !it->contains("max") || !(*it)["min"].is_number() || !(*it)["max"].is_number())
          return bad(m.id, path, "gives no \"min\" and \"max\" for \"" + s.name + "\".", "A number needs its range.");
        s.lo = (*it)["min"].get<double>();
        s.hi = (*it)["max"].get<double>();
        if (s.lo > s.hi)
          return bad(m.id, path, "has \"min\" above \"max\" for \"" + s.name + "\".", "Swap them.");
      }
      if (s.type == SettingDecl::Type::choice) {
        const auto options = it->find("options");
        if (options == it->end() || !options->is_array() || options->empty())
          return bad(m.id, path + "/options", "gives no options for the choice \"" + s.name + "\".", "List the values it accepts.");
        for (const json &o : *options) {
          if (!o.is_string())
            return bad(m.id, path + "/options", "has an option of \"" + s.name + "\" that is not text.", "Options are strings.");
          s.options.push_back(o.get<std::string>());
        }
      }
      if (const auto def = it->find("default"); def != it->end()) {
        s.def = *def;
        if (const std::string wrong = setting_problem(s, s.def); !wrong.empty())
          return bad(m.id, path + "/default", "has a default for \"" + s.name + "\" that " + wrong + ".", "Pick a default inside the range.");
      }
      m.settings.push_back(std::move(s));
    }
  }
  return m;
}

void register_model(ModelDecl declaration) {
  Registry &r = registry();
  std::lock_guard lock(r.mutex);
  std::string id = declaration.id;
  r.models[std::move(id)] = std::make_unique<ModelDecl>(std::move(declaration));
}

const ModelDecl *find_model(std::string_view id) {
  Registry &r = registry();
  std::lock_guard lock(r.mutex);
  const auto it = r.models.find(id);
  return it == r.models.end() ? nullptr : it->second.get();
}

std::vector<std::string> model_ids() {
  Registry &r = registry();
  std::lock_guard lock(r.mutex);
  std::vector<std::string> out;
  for (const auto &[id, model] : r.models)
    out.push_back(id);
  return out;
}

void clear_models() {
  Registry &r = registry();
  std::lock_guard lock(r.mutex);
  r.models.clear();
}

std::string setting_problem(const SettingDecl &s, const json &v) {
  switch (s.type) {
  case SettingDecl::Type::boolean:
    return v.is_boolean() ? "" : "must be true or false";
  case SettingDecl::Type::text:
    return v.is_string() ? "" : "must be text";
  case SettingDecl::Type::choice: {
    if (v.is_string() && std::find(s.options.begin(), s.options.end(), v.get_ref<const std::string &>()) != s.options.end())
      return "";
    return "must be one of: " + joined(s.options);
  }
  case SettingDecl::Type::integer:
    if (!v.is_number_integer())
      return "must be a whole number from " + number_text(s.lo) + " to " + number_text(s.hi);
    [[fallthrough]];
  case SettingDecl::Type::number:
    if (!v.is_number())
      return "must be a number from " + number_text(s.lo) + " to " + number_text(s.hi);
    if (const double x = v.get<double>(); x < s.lo || x > s.hi)
      return "is " + number_text(x) + "; the model takes " + number_text(s.lo) + " to " + number_text(s.hi);
    return "";
  }
  return "";
}

std::string input_problem(const ModelDecl &m, std::string_view input, const json &v) {
  if (!v.is_number())
    return ""; // not a number: the port's own type check reports it
  const double x = v.get<double>();
  if (input == "seconds") {
    if (x < m.seconds_min || (m.seconds_max != 0.0 && x > m.seconds_max) || x <= 0.0)
      return "is " + number_text(x) + " s; the model makes " + number_text(m.seconds_min) + " to " +
             (m.seconds_max != 0.0 ? number_text(m.seconds_max) + " s" : "any length");
  } else if (input == "width" || input == "height") {
    if (x < m.size_multiple || std::fmod(x, double(m.size_multiple)) != 0.0)
      return "is " + number_text(x) + "; the model needs a multiple of " + std::to_string(m.size_multiple);
  }
  return "";
}

std::pair<int64_t, int64_t> scaled_size(int64_t width, int64_t height, int64_t pixels) {
  if (pixels <= 0 || width <= 0 || height <= 0)
    return {width, height};
  const double fit = std::sqrt(double(pixels) / (double(width) * double(height)));
  const auto even = [](double v) { return std::max<int64_t>(2, int64_t(std::llround(v / 2.0)) * 2); };
  return {even(double(width) * fit), even(double(height) * fit)};
}

std::pair<int64_t, int64_t> fit_size(const ModelDecl &model, int64_t width, int64_t height) {
  const int64_t grid = std::max(1, model.size_multiple);
  const auto on_grid = [&](double v) { return std::max<int64_t>(grid, int64_t(std::llround(v / double(grid))) * grid); };
  int64_t w = on_grid(double(width)), h = on_grid(double(height));
  if (model.max_pixels > 0 && w * h > model.max_pixels) {
    const double fit = std::sqrt(double(model.max_pixels) / (double(width) * double(height)));
    w = std::max<int64_t>(grid, int64_t(std::floor(double(width) * fit / double(grid))) * grid);
    h = std::max<int64_t>(grid, int64_t(std::floor(double(height) * fit / double(grid))) * grid);
  }
  return {w, h};
}

namespace {
void models_in(const json &library, const json &workflow, const std::string &owner, const std::function<bool(std::string_view)> &installed,
               std::vector<Problem> &out, std::vector<std::string> &seen);
}

void check_models(const json &library, const json &workflow, const std::string &owner, const std::function<bool(std::string_view)> &installed,
                  std::vector<Problem> &out) {
  std::vector<std::string> seen;
  models_in(library, workflow, owner, installed, out, seen);
}

namespace {
// One workflow, then the library workflows it uses as nodes, each looked at once.
void models_in(const json &library, const json &workflow, const std::string &owner, const std::function<bool(std::string_view)> &installed,
               std::vector<Problem> &out, std::vector<std::string> &seen) {
  const auto nodes = workflow.is_object() ? workflow.find("nodes") : workflow.end();
  if (!workflow.is_object() || nodes == workflow.end() || !nodes->is_object())
    return;
  for (auto it = nodes->begin(); it != nodes->end(); ++it) {
    const auto kind = it->find("kind");
    if (kind != it->end() && kind->is_string() && is_workflow_kind(kind->get_ref<const std::string &>())) {
      const auto inner = it->find("workflow");
      if (inner != it->end() && inner->is_string() && library.is_object() && std::find(seen.begin(), seen.end(), inner->get<std::string>()) == seen.end()) {
        seen.push_back(inner->get<std::string>());
        if (const auto wf = library.find(inner->get<std::string>()); wf != library.end())
          models_in(library, *wf, inner->get<std::string>(), installed, out, seen);
      }
      continue;
    }
    const KindDef *def = kind != it->end() && kind->is_string() ? find_kind(kind->get_ref<const std::string &>()) : nullptr;
    if (!def || !def->runs_model)
      continue;
    const auto mit = it->find("model");
    const std::string model = mit != it->end() && mit->is_string() ? mit->get<std::string>() : std::string();
    const std::string path = it.key() + "/model";
    if (model.empty()) {
      out.push_back({"G_MODEL_UNSET", path, owner, "Node " + it.key() + " (" + def->title + ") has no model chosen.",
                     "Pick a model for the node."});
      continue;
    }
    const ModelDecl *decl = find_model(model);
    if (!decl)
      out.push_back({"G_MODEL_UNKNOWN", path, owner,
                     "Node " + it.key() + " uses the model \"" + model + "\", which this version of Attome does not know.",
                     "Pick a model from the Models panel, or update Attome."});
    else if (decl->needs_files && !(installed && installed(model)))
      out.push_back({"G_MODEL_MISSING", path, owner,
                     "Node " + it.key() + " uses the model \"" + model + "\", which is not installed on this computer.",
                     "Download it in the Models panel (attome models fetch " + model + "), or show Attome the folder that already has it."});
  }
}
} // namespace

} // namespace atm::gen
