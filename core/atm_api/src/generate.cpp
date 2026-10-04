#include "generate.hpp"

#include <algorithm>
#include <chrono>
#include <map>
#include <set>

#include "atm/base/id.hpp"
#include "atm/base/profiler.hpp"
#include "atm/base/time.hpp"
#include "atm/storage/file.hpp"

namespace atm::api {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::string to_utf8(const fs::path &p) {
  const std::u8string s = p.generic_u8string();
  return std::string(s.begin(), s.end());
}

bool is_made(const json &v) { return v.is_object() && v.contains("key") && v.contains("port"); }

fs::path made_file(const fs::path &cache, const json &made) {
  return step_dir(cache, made.value("key", std::string())) / gen::output_file(made.value("port", std::string()));
}

bool in_cache(const fs::path &cache, const std::string &key) { return storage::exists(step_dir(cache, key) / "result.json"); }

Error engine_error(const char *rule, std::string message, std::string hint) {
  Error e;
  e.code = ErrorCode::Internal;
  e.rule = rule;
  e.message = std::move(message);
  e.hint = std::move(hint);
  return e;
}

} // namespace

fs::path step_dir(const fs::path &cache, const std::string &key) {
  return cache / (key.rfind("b3:", 0) == 0 ? key.substr(3) : key);
}

gen::Provider *provider_for(const std::vector<std::shared_ptr<gen::Provider>> &providers, std::string_view model, std::string_view kind) {
  for (const auto &p : providers)
    if (p->offers(model, kind))
      return p.get();
  return nullptr;
}

StepCount count_steps(const GenRun &run) {
  StepCount n;
  std::set<std::string> seen;
  for (const GenClip &clip : run.clips)
    for (const gen::Step &s : gen::steps(run.workflows, clip.workflow, clip.inputs, run.context))
      if (seen.insert(s.key).second) {
        ++n.total;
        n.cached += in_cache(run.dir, s.key) ? 1 : 0;
      }
  return n;
}

std::vector<GenOutcome> run_generation(const GenRun &run, const GenProgress &progress) {
  ATM_PROFILE_SCOPE("gen.run");
  std::vector<GenOutcome> outcomes;
  std::map<std::string, std::string> ended; // clip -> how it ended
  const auto cancelled = [&] { return progress.cancel && progress.cancel->load(); };
  int index = 0;
  for (const GenClip &clip : run.clips) {
    ++index;
    GenOutcome out;
    out.clip = clip.id;
    out.name = clip.name;
    const auto started = std::chrono::steady_clock::now();
    const std::string label = clip.name + " (" + std::to_string(index) + " of " + std::to_string(run.clips.size()) + ")";
    const auto finish = [&](const char *state) {
      out.state = state;
      out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
      ended[clip.id] = state;
      if (progress.on_clip)
        progress.on_clip(out);
      outcomes.push_back(out);
    };
    if (cancelled()) {
      finish("cancelled");
      continue;
    }
    // A clip does not run when a clip it takes from, in this run, did not finish.
    for (const std::string &d : clip.depends)
      if (const auto it = ended.find(d); it != ended.end() && it->second != "done")
        out.why = "the clip it starts from " + (it->second == "failed" ? std::string("failed") : "did not run");
    if (!out.why.empty()) {
      finish("skipped");
      continue;
    }
    const std::vector<gen::Step> steps = gen::steps(run.workflows, clip.workflow, clip.inputs, run.context);
    if (steps.empty()) {
      out.error = engine_error("G_PLAN", "The steps of " + clip.name + " cannot be worked out.", "Check the clip's workflow with project.validate.");
      finish("failed");
      continue;
    }
    bool ok = true;
    for (const gen::Step &step : steps) {
      ATM_PROFILE_SCOPE("gen.step");
      if (cancelled()) {
        ok = false;
        finish("cancelled");
        break;
      }
      if (in_cache(run.dir, step.key)) {
        ++out.cached;
        if (progress.done)
          progress.done->fetch_add(1);
        continue;
      }
      const std::string kind = step.what.value("kind", std::string()), model = step.what.value("model", std::string());
      gen::Provider *provider = provider_for(run.providers, model, kind);
      if (!provider) {
        out.error = engine_error("G_NO_ENGINE", "No engine on this computer runs \"" + kind + "\" for the model " + model + ".",
                                 "Attome's own engine for this model arrives in a later version.");
        ok = false;
        finish("failed");
        break;
      }
      const fs::path final_dir = step_dir(run.dir, step.key), part = fs::path(final_dir).concat(".part");
      std::error_code ec;
      fs::remove_all(part, ec);
      if (auto made = storage::make_dirs(part); !made) {
        out.error = made.error();
        ok = false;
        finish("failed");
        break;
      }
      gen::StepRequest request;
      request.run_id = new_id("run");
      request.kind = kind;
      request.model = model;
      request.settings = step.what.value("settings", json::object());
      request.cancel = progress.cancel;
      request.inputs = json::object();
      const json inputs = step.what.value("inputs", json::object());
      for (auto it = inputs.begin(); it != inputs.end(); ++it) { // results of other steps become their files
        if (is_made(*it)) {
          request.inputs[it.key()] = to_utf8(made_file(run.dir, *it));
        } else if (it->is_array()) {
          json list = json::array();
          for (const json &e : *it)
            list.push_back(is_made(e) ? json(to_utf8(made_file(run.dir, e))) : e);
          request.inputs[it.key()] = std::move(list);
        } else {
          request.inputs[it.key()] = *it;
        }
      }
      if (const gen::KindDef *def = gen::find_kind(kind))
        for (const gen::PortDef &port : def->outputs)
          request.outputs[port.name] = to_utf8(part / gen::output_file(port.name));
      request.progress = [&](std::string_view phase, int at, int of) {
        if (progress.on_detail)
          progress.on_detail(label + ": " + std::string(phase) + (of > 0 ? " " + std::to_string(at) + " of " + std::to_string(of) : ""));
      };
      if (progress.on_detail)
        progress.on_detail(label + ": " + kind);
      const auto result = provider->run(request);
      if (!result) {
        fs::remove_all(part, ec);
        ok = false;
        if (result.error().rule == "E_CANCELLED" || cancelled()) {
          finish("cancelled");
        } else {
          out.error = result.error();
          finish("failed");
        }
        break;
      }
      json files = json::object();
      for (const auto &[port, path] : request.outputs)
        if (storage::exists(part / gen::output_file(port)))
          files[port] = gen::output_file(port);
      json record = {{"key", step.key}, {"kind", kind}, {"model", model}, {"engine", provider->name()}, {"outputs", std::move(files)},
                     {"made", utc_now_iso8601()}, {"seconds", result->seconds}};
      auto written = storage::atomic_write(part / "result.json", record.dump(1) + "\n");
      fs::remove_all(final_dir, ec);
      fs::rename(part, final_dir, ec);
      if (!written || ec) {
        out.error = written ? engine_error("G_CACHE", "The result of a step could not be put in the cache: " + ec.message() + ".",
                                           "Check that the project folder can be written to.")
                            : written.error();
        fs::remove_all(part, ec);
        ok = false;
        finish("failed");
        break;
      }
      ++out.ran;
      if (progress.done)
        progress.done->fetch_add(1);
    }
    if (!ok)
      continue;
    // The Take: what each exposed output of the clip's workflow is, and where its file lies.
    json outputs = json::object();
    for (const gen::Port &port : gen::workflow_ports(run.workflows, clip.workflow).outputs) {
      const gen::Made made = gen::output_key(run.workflows, clip.workflow, clip.inputs, port.name, run.context);
      const fs::path file = step_dir(run.dir, made.key) / gen::output_file(made.port);
      if (!made.key.empty() && storage::exists(file))
        outputs[port.name] = {{"key", made.key}, {"port", made.port}, {"path", to_utf8(file)}};
    }
    out.take = {{"key", clip.key}, {"made", utc_now_iso8601()}, {"inputs", clip.written}, {"outputs", std::move(outputs)}};
    finish("done");
  }
  return outcomes;
}

} // namespace atm::api
