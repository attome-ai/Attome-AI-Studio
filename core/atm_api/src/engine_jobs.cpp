// Jobs and daemon Tools: jobs.get/cancel, profile.*, daemon.*.
#include "engine_impl.hpp"

namespace atm::api {

 // defined after the table
Result<json> Engine::Impl::daemon_hello(const json &) {
  return json{{"protocol", kProtocolVersion},
              {"engine", kEngineVersion},
              {"schema", json::array({"1.0.0"})},
              {"pid", process_id()}};
}

Result<json> Engine::Impl::daemon_status(const json &) {
  json open = json::array();
  for (const auto &[key, pr] : projects)
    open.push_back({{"id", pr->doc.id()}, {"path", to_utf8(pr->dir)}, {"revision", pr->revision},
                    {"objects", pr->doc.object_count()}, {"unsaved", pr->dirty}});
  return json{{"engine", kEngineVersion},
              {"pid", process_id()},
              {"uptime_s", std::chrono::duration<double>(Clock::now() - started).count()},
              {"fsync", cfg.fsync},
              {"profiling", prof::enabled()},
              {"projects", std::move(open)}};
}

Result<json> Engine::Impl::daemon_shutdown(const json &) {
  shutdown = true;
  return json{{"stopping", true}};
}

Result<std::shared_ptr<Job>> Engine::Impl::job_param(const json &params) {
  ATM_TRY(const std::string *id, string_param(params, "job_id"));
  const auto it = jobs.find(*id);
  if (it == jobs.end())
    return fail(ErrorCode::NotFound, "R_NO_JOB", "There is no job \"" + *id + "\".");
  return it->second;
}

Result<json> Engine::Impl::jobs_get(const json &params) {
  ATM_TRY(std::shared_ptr<Job> job, job_param(params));
  static constexpr const char *kStates[] = {"running", "done", "failed", "cancelled"};
  const int state = job->state.load();
  const int64_t done = job->units_done.load(), total = job->units_total.load();
  json out = {{"job", job->id},
              {"kind", job->kind},
              {"state", kStates[state]},
              {"progress", total > 0 ? double(done) / double(total) : 0.0},
              {"frames_done", done},
              {"frames_total", total},
              {"units_done", done},
              {"units_total", total},
              {"unit", job->kind == "models.fetch" ? "bytes" : job->kind == "gen.run" ? "steps" : "frames"},
              {"output", job->output}};
  std::lock_guard lock(job->mutex);
  const double seconds = state == Job::running
                             ? std::chrono::duration<double>(Clock::now() - job->started).count()
                             : job->seconds;
  out["seconds"] = seconds;
  out["fps"] = seconds > 0.0 ? double(done) / seconds : 0.0;
  if (job->kind == "models.fetch")
    out["bytes_per_second"] = seconds > 0.0 ? double(job->fetched.load()) / seconds : 0.0;
  if (state == Job::failed)
    out["error"] = error_to_json(job->error);
  if (!job->warning.empty())
    out["warning"] = job->warning;
  if (!job->detail.empty())
    out["detail"] = job->detail;
  if (!job->rendered_on.empty())
    out["rendered_on"] = job->rendered_on; // where the effects of a render ran
  if (!job->node.empty())
    out["node"] = {{"id", job->node}, {"at", job->node_at}, {"of", job->node_of}};
  if (!job->result.is_null())
    out["result"] = job->result;
  if (!job->encoder.empty())
    out["encoder"] = job->encoder;
  if (state != Job::running && job->thread.joinable())
    job->thread.join();
  return out;
}

Result<json> Engine::Impl::jobs_cancel(const json &params) {
  ATM_TRY(std::shared_ptr<Job> job, job_param(params));
  job->cancel.store(true);
  return json{{"job", job->id}, {"cancelling", true}};
}

Result<json> Engine::Impl::profile_get(const json &params) {
  json snap = prof::snapshot();
  if (params.value("reset", false))
    prof::reset();
  return snap;
}

Result<json> Engine::Impl::profile_reset(const json &) {
  prof::reset();
  return json{{"reset", true}};
}

Result<json> Engine::Impl::profile_set(const json &params) {
  const auto on = params.find("enabled");
  if (on == params.end() || !on->is_boolean())
    return bad_param("enabled", "is required and must be true or false");
  prof::set_enabled(on->get<bool>());
  return json{{"enabled", prof::enabled()}};
}

} // namespace atm::api
