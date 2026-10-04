#include "atm/api/mcp.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <thread>
#include <unordered_map>

#include "atm/api/engine.hpp"
#include "atm/storage/file.hpp"

namespace atm::api {
namespace {

constexpr const char *kLatestProtocol = "2025-06-18";
constexpr const char *kProtocols[] = {"2025-06-18", "2025-03-26", "2024-11-05"};

constexpr const char *kInstructions =
    "Attome edits video projects (.attome folders) and renders them to MP4.\n"
    "Workflow: guide_get (topic \"timeline\" first) -> project_create (or use an existing .attome folder) -> "
    "media_import the files -> timeline_edit to build the cut in one call (clips, text, dissolves, blur, music) -> "
    "project_inspect with level "
    "\"tracks\" to read the IDs -> see_contact_sheet or see_frames to check the result by eye -> render_sequence -> "
    "jobs_wait until the job is done.\n"
    "Always pass absolute paths. Times accept \"12.5s\", \"375@30\" (frames@rate) and SMPTE timecode. Every edit can "
    "be undone with project_undo. The user may have the same project open in the Attome editor; edits show up there "
    "at once.";

std::string base64(const std::string &bytes) {
  static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((bytes.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < bytes.size(); i += 3) {
    const uint32_t v = uint32_t(uint8_t(bytes[i])) << 16 | uint32_t(uint8_t(bytes[i + 1])) << 8 | uint8_t(bytes[i + 2]);
    out += kAlphabet[v >> 18];
    out += kAlphabet[(v >> 12) & 63];
    out += kAlphabet[(v >> 6) & 63];
    out += kAlphabet[v & 63];
  }
  if (i < bytes.size()) {
    const bool two = i + 1 < bytes.size();
    const uint32_t v = uint32_t(uint8_t(bytes[i])) << 16 | (two ? uint32_t(uint8_t(bytes[i + 1])) << 8 : 0u);
    out += kAlphabet[v >> 18];
    out += kAlphabet[(v >> 12) & 63];
    out += two ? kAlphabet[(v >> 6) & 63] : '=';
    out += '=';
  }
  return out;
}

std::string mcp_name(std::string name) {
  std::replace(name.begin(), name.end(), '.', '_');
  return name;
}

json rpc_error(const json &id, int code, std::string message) {
  return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", std::move(message)}}}};
}

json text_result(const std::string &text, bool is_error = false) {
  json out = {{"content", json::array({{{"type", "text"}, {"text", text}}})}};
  if (is_error)
    out["isError"] = true;
  return out;
}

// A failed Tool as a tool result the agent can read and act on (MCP: Tool errors are results, not protocol errors).
json error_result(const json &error) {
  std::string text = "Error: " + error.value("message", std::string("unknown error"));
  const json data = error.value("data", json::object());
  if (data.is_object()) {
    if (const auto hint = data.find("hint"); hint != data.end() && hint->is_string() && !hint->get_ref<const std::string &>().empty())
      text += "\nHint: " + hint->get<std::string>();
    if (const auto errors = data.find("errors"); errors != data.end() && errors->is_array())
      for (const json &e : *errors)
        text += "\n- " + e.value("message", std::string()) + (e.contains("hint") ? "  (" + e.value("hint", std::string()) + ")" : "");
  }
  text += "\n" + error.dump(-1, ' ', false, json::error_handler_t::replace);
  return text_result(text, true);
}

} // namespace

struct McpServer::Impl {
  ToolCaller call;
  std::unordered_map<std::string, std::string> tools; // MCP name -> engine Tool name
  json tool_list;                                     // MCP tools/list result, built once

  Result<void> load_tools() {
    if (!tool_list.is_null())
      return {};
    const json outcome = call("tools.list", json::object());
    if (!outcome.value("ok", false))
      return fail(ErrorCode::Internal, "MCP_TOOLS", "The engine did not list its Tools: " +
                                                        outcome.value("error", json::object()).value("message", ""));
    json list = json::array();
    for (const json &tool : outcome["result"]["tools"]) {
      const std::string name = tool.value("name", "");
      // The daemon's own controls and the registry listing are not for agents.
      // Nor is starting a download of many gigabytes: that is the person's decision (models.list stays visible).
      if (tool.value("group", "") == "daemon" || name == "tools.list" || name == "models.fetch")
        continue;
      tools[mcp_name(name)] = name;
      list.push_back({{"name", mcp_name(name)},
                      {"description", tool.value("summary", "")},
                      {"inputSchema", tool.value("params", json{{"type", "object"}})},
                      {"annotations", {{"readOnlyHint", !tool.value("mutating", false)}}}});
    }
    // jobs.wait is a long poll; the daemon's single writer thread must not block on it, so it is served here.
    list.push_back({{"name", "jobs_wait"},
                    {"description", "Wait until a job (e.g. a render) finishes, at most timeout_s seconds (default "
                                    "25, at most 30), and return its state. Call again while the state is "
                                    "\"running\"."},
                    {"inputSchema",
                     {{"type", "object"},
                      {"properties", {{"job_id", {{"type", "string"}}}, {"timeout_s", {{"type", "number"}}}}},
                      {"required", json::array({"job_id"})}}},
                    {"annotations", {{"readOnlyHint", true}}}});
    tool_list = {{"tools", std::move(list)}};
    return {};
  }

  json jobs_wait(const json &args) {
    const double timeout = std::clamp(args.value("timeout_s", 25.0), 0.0, 30.0);
    const json params = {{"job_id", args.value("job_id", std::string())}};
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout);
    for (;;) {
      json outcome = call("jobs.get", params);
      if (!outcome.value("ok", false))
        return error_result(outcome.value("error", json::object()));
      if (outcome["result"].value("state", "") != "running" || std::chrono::steady_clock::now() >= until)
        return text_result(outcome["result"].dump(-1, ' ', false, json::error_handler_t::replace));
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
  }

  // The Tool's JSON as text, then each picture it wrote (see.*) as image content.
  static json to_result(const json &result) {
    json out = text_result(result.dump(-1, ' ', false, json::error_handler_t::replace));
    const auto images = result.find("images");
    if (images == result.end() || !images->is_array())
      return out;
    for (const json &image : *images) {
      const std::string path = image.value("path", "");
      if (path.empty())
        continue;
      auto bytes = storage::read_file(std::filesystem::path(std::u8string(path.begin(), path.end())));
      if (!bytes || bytes->size() > kMaxInlineImage)
        continue; // the path in the text is still there
      out["content"].push_back({{"type", "image"}, {"data", base64(*bytes)}, {"mimeType", "image/jpeg"}});
    }
    return out;
  }

  json tools_call(const json &params) {
    const std::string name = params.value("name", "");
    json args = params.value("arguments", json::object());
    if (!args.is_object())
      args = json::object();
    if (name == "jobs_wait")
      return jobs_wait(args);
    const auto it = tools.find(name);
    if (it == tools.end())
      return json(nullptr);
    const json outcome = call(it->second, args);
    if (!outcome.value("ok", false))
      return error_result(outcome.value("error", json::object()));
    return to_result(outcome["result"]);
  }
};

McpServer::McpServer(ToolCaller call) : impl_(std::make_unique<Impl>()) { impl_->call = std::move(call); }

McpServer::~McpServer() = default;

json McpServer::handle(const json &message) {
  if (message.is_array()) { // a batch (protocol 2025-03-26)
    json out = json::array();
    for (const json &one : message)
      if (json r = handle(one); !r.is_null())
        out.push_back(std::move(r));
    return out.empty() ? json(nullptr) : out;
  }
  if (!message.is_object() || !message.contains("method") || !message["method"].is_string())
    return message.is_object() && message.contains("id") ? rpc_error(message["id"], -32600, "Invalid request.")
                                                         : json(nullptr); // a response to us, or junk
  const auto id_it = message.find("id");
  if (id_it == message.end())
    return nullptr; // notifications (initialized, cancelled …) need no answer
  const json &id = *id_it;
  const std::string method = message["method"];
  const json params = message.value("params", json::object());
  const auto reply = [&](json result) { return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}; };

  if (method == "initialize") {
    std::string version = params.value("protocolVersion", std::string(kLatestProtocol));
    if (std::find(std::begin(kProtocols), std::end(kProtocols), version) == std::end(kProtocols))
      version = kLatestProtocol;
    return reply({{"protocolVersion", version},
                  {"capabilities", {{"tools", {{"listChanged", false}}}}},
                  {"serverInfo", {{"name", "attome"}, {"title", "Attome"}, {"version", kEngineVersion}}},
                  {"instructions", kInstructions}});
  }
  if (method == "ping")
    return reply(json::object());
  if (method == "tools/list") {
    if (auto loaded = impl_->load_tools(); !loaded)
      return rpc_error(id, -32603, loaded.error().message);
    return reply(impl_->tool_list);
  }
  if (method == "tools/call") {
    if (auto loaded = impl_->load_tools(); !loaded)
      return rpc_error(id, -32603, loaded.error().message);
    json result = impl_->tools_call(params);
    if (result.is_null())
      return rpc_error(id, -32602, "There is no tool named \"" + params.value("name", std::string()) + "\".");
    return reply(std::move(result));
  }
  return rpc_error(id, -32601, "Method not found: " + method);
}

} // namespace atm::api
