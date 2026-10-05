#pragma once
// Running a generation plan: for each clip, the steps of its workflow in order; a step whose result is already in the
// cache is not run again; a finished clip becomes a Take. Runs on a job thread, on its own copy of the data.
//
// The cache is <project>/.attome/gen/<key>/: the files a step wrote (gen::output_file names them) and result.json.
// A step writes into "<key>.part" and the folder gets its name only when the step is done, so a folder under its own
// name is always complete.

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "atm/gen/keys.hpp"
#include "atm/gen/provider.hpp"

namespace atm::api {

struct GenClip {
  std::string id, name, key;
  nlohmann::json instance; // the clip's own workflow
  nlohmann::json written;  // the clip's inputs as the document has them (kept in the Take)
  nlohmann::json inputs;   // the values the clip gives its Exposed Inputs
  std::vector<std::string> depends;
  gen::ClipFacts facts;    // what its Input nodes read: the canvas, its Duration
  std::map<std::string, gen::Made> references; // what its Clip Reference nodes gave (gen::ClipPlan::references)
};

struct GenRun {
  nlohmann::json library;  // the project's library of Clip Workflows, for workflows used as nodes
  std::vector<GenClip> clips; // in run order
  std::filesystem::path dir;  // the cache folder
  std::filesystem::path project; // the project folder: a Take records its files relative to it
  std::vector<std::shared_ptr<gen::Provider>> providers;
  gen::KeyContext context; // what is shared by the clips: the model identities and the project's Variables
  // The context of one clip: the shared one with the clip's own facts, and its references as the plan recorded them.
  gen::KeyContext context_of(const GenClip &clip) const;
};

struct GenOutcome {
  std::string clip, name;
  std::string state; // "done", "failed", "skipped", "cancelled"
  std::string why;   // for skipped
  Error error;       // for failed
  std::string node;  // for failed: the node of the clip's workflow it stopped at (its ID), when it is known
  nlohmann::json take; // for done
  int ran = 0, cached = 0; // steps
  double seconds = 0.0;
};

struct GenProgress {
  std::atomic<int64_t> *done = nullptr;                // steps finished, cached ones included
  const std::atomic<bool> *cancel = nullptr;
  std::function<void(const std::string &)> on_detail;  // "Shot (2 of 5): sampling 3 of 8"
  std::function<void(const GenOutcome &)> on_clip;     // after each clip, in order
};

std::filesystem::path step_dir(const std::filesystem::path &cache, const std::string &key);
// How many steps the run has in all, and how many of them are already in the cache.
struct StepCount {
  int total = 0, cached = 0;
};
StepCount count_steps(const GenRun &run);
// The provider that runs this kind of step for this model, or null.
gen::Provider *provider_for(const std::vector<std::shared_ptr<gen::Provider>> &providers, std::string_view model, std::string_view kind);

std::vector<GenOutcome> run_generation(const GenRun &run, const GenProgress &progress);

} // namespace atm::api
