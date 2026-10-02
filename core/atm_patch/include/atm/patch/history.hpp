#pragma once
// M4 History: ChangeSets form an undo tree. A new edit after an undo starts a sibling branch; nothing is discarded.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "atm/patch/patch.hpp"

namespace atm::patch {

struct ChangeSet {
  std::string id;  // cs_…
  int parent = -1; // index in History::nodes(); -1 = the state the epoch started from
  uint64_t seq = 0;
  std::string task, label, time_utc;
  json forward, inverse;
  std::vector<int> children;
};

class History {
public:
  int head() const { return head_; }
  void set_head(int index) { head_ = index; }
  const std::vector<ChangeSet> &nodes() const { return nodes_; }
  int find(std::string_view id) const;
  const std::vector<int> &children_of(int index) const { return index < 0 ? roots_ : nodes_[size_t(index)].children; }

  // Adds a ChangeSet under `parent` and makes it HEAD. Returns its index.
  int add(ChangeSet cs, int parent);

  static std::string to_record(const ChangeSet &cs, std::string_view parent_id); // journal payload {"k":"cs",…}
  static ChangeSet from_record(const json &record);

private:
  std::vector<ChangeSet> nodes_;
  std::vector<int> roots_;
  int head_ = -1;
};

} // namespace atm::patch
