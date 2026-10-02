#include "atm/patch/history.hpp"

namespace atm::patch {

int History::find(std::string_view id) const {
  for (size_t i = nodes_.size(); i-- > 0;) // recent ChangeSets are asked for most
    if (nodes_[i].id == id)
      return int(i);
  return -1;
}

int History::add(ChangeSet cs, int parent) {
  const int index = int(nodes_.size());
  cs.parent = parent;
  cs.seq = uint64_t(index) + 1;
  nodes_.push_back(std::move(cs));
  if (parent < 0)
    roots_.push_back(index);
  else
    nodes_[size_t(parent)].children.push_back(index);
  head_ = index;
  return index;
}

std::string History::to_record(const ChangeSet &cs, std::string_view parent_id) {
  // Built as text so the forward and inverse patches are serialized once and never copied.
  std::string r;
  r.reserve(256);
  r += "{\"k\":\"cs\",\"id\":";
  r += json(cs.id).dump();
  r += ",\"parent\":";
  r += parent_id.empty() ? std::string("null") : json(parent_id).dump();
  r += ",\"task\":";
  r += json(cs.task).dump();
  r += ",\"label\":";
  r += json(cs.label).dump();
  r += ",\"time\":";
  r += json(cs.time_utc).dump();
  r += ",\"fwd\":";
  r += cs.forward.dump();
  r += ",\"inv\":";
  r += cs.inverse.dump();
  r += '}';
  return r;
}

ChangeSet History::from_record(const json &r) {
  ChangeSet cs;
  cs.id = r.value("id", "");
  cs.task = r.value("task", "");
  cs.label = r.value("label", "");
  cs.time_utc = r.value("time", "");
  cs.forward = r.value("fwd", json::array());
  cs.inverse = r.value("inv", json::array());
  return cs;
}

} // namespace atm::patch
