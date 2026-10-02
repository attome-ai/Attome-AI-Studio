#pragma once
// MCP server (MODULES §AI Host, F1-E10-T3/T4): exposes the engine's Tools to an AI agent over the Model Context
// Protocol. It proxies the Tool registry, so names, descriptions and parameter schemas are never written twice:
// "project.patch" is offered as "project_patch" with the schema from tools.list. see.* pictures are attached as image
// content. The transport (newline-delimited JSON-RPC on stdio) lives in the caller.

#include <functional>
#include <memory>
#include <string>

#include "atm/base/error.hpp"

namespace atm::api {

using json = nlohmann::json;

// Runs one Tool and returns its outcome: {"ok":true,"result":…} or {"ok":false,"error":{code,message,data}}.
using ToolCaller = std::function<json(const std::string &tool, const json &params)>;

class McpServer {
public:
  explicit McpServer(ToolCaller call);
  ~McpServer();

  // One JSON-RPC message from the client in, the response out (null for notifications).
  json handle(const json &message);

  static constexpr size_t kMaxInlineImage = 256 * 1024; // larger pictures are returned as file paths only

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace atm::api
