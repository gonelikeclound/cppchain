// cppchain —— mcp/manager.cpp
// MCP 连接管理器实现：三路发现（so:// file:// url://）→ 统一 JSON → parse；懒建 client + 握手。
#include "cppchain/mcp/manager.hpp"

#include <dlfcn.h>

#include <filesystem>
#include <fstream>
#include <utility>

#include "cppchain/core/cancel.hpp"
#include "cppchain/tool/http_json.hpp"

namespace cppchain::mcp {

using Json = nlohmann::json;

namespace {

// so://<so-name>:<fn> → 从 lib<so-name>.so dlsym `mcp_list_<fn>`，返回统一 JSON 字符串。
// 句柄只用于取一次列表（字符串 parse 进 owned Json 后不再需要）→ 取完即 dlclose。
std::expected<Json, std::string> discover_from_so(std::string const& lib_name,
                                                  std::string const& fn,
                                                  std::vector<std::string> const& dirs) {
  std::string const libfile = "lib" + lib_name + ".so";
  std::string lib_path = libfile;
  for (auto const& d : dirs) {
    std::filesystem::path const cand = std::filesystem::path(d) / libfile;
    if (std::filesystem::exists(cand)) { lib_path = cand.string(); break; }
  }
  void* const handle = dlopen(lib_path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr)
    return std::unexpected("dlopen '" + lib_path + "' 失败（so:// 发现源）");
  std::string const sym = "mcp_list_" + fn;
  void* const addr = dlsym(handle, sym.c_str());
  if (addr == nullptr) {
    dlclose(handle);
    return std::unexpected("so:// 发现源 '" + lib_path + "' 取不到符号 '" + sym + "'");
  }
  auto const getter = reinterpret_cast<char const* (*)()>(addr);
  char const* raw = getter();
  if (raw == nullptr) {
    dlclose(handle);
    return std::unexpected("mcp_list_" + fn + " 返回空");
  }
  auto j = Json::parse(raw, nullptr, /*allow_exceptions=*/false);
  dlclose(handle);
  if (j.is_discarded()) return std::unexpected("mcp_list_" + fn + " 返回非法 JSON");
  return j;
}

std::expected<Json, std::string> discover_from_file(std::string const& path) {
  std::ifstream f(path);
  if (!f) return std::unexpected("file:// 发现源打不开 '" + path + "'");
  std::string const content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  auto j = Json::parse(content, nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded()) return std::unexpected("file:// 发现源 '" + path + "' 非法 JSON");
  return j;
}

std::expected<Json, std::string> discover_from_url(http::Transport& transport,
                                                   std::string const& url) {
  http::HttpRequest req;
  req.url = url;
  req.method = "GET";
  auto resp = tool::http_json(transport, req, core::CancelToken{});
  if (!resp) return std::unexpected("url:// 发现源 '" + url + "' 传输失败：" + resp.error().message);
  if (resp->body.is_discarded() || resp->body.is_null())
    return std::unexpected("url:// 发现源 '" + url + "' 响应非 JSON");
  return resp->body;
}

}  // namespace

std::expected<std::vector<McpServerInfo>, std::string> MCPConnectionManager::discover(
    std::string_view from) {
  std::expected<Json, std::string> raw;
  if (from.starts_with("so://")) {
    // so://<so-name>:<fn>
    std::string const spec(from.substr(5));
    auto const colon = spec.find(':');
    if (colon == std::string::npos)
      return std::unexpected("so:// 发现源格式应为 so://<so-name>:<mcp_list_fn>");
    raw = discover_from_so(spec.substr(0, colon), spec.substr(colon + 1), so_dirs_);
  } else if (from.starts_with("file://")) {
    raw = discover_from_file(std::string(from.substr(7)));
  } else if (from.starts_with("url://")) {
    raw = discover_from_url(*transport_, std::string(from.substr(6)));
  } else {
    return std::unexpected("未知发现源 '" + std::string(from) +
                           "'（支持 so:// / file:// / url://）");
  }
  if (!raw) return std::unexpected(raw.error());
  return parse_mcp_server_list(*raw);  // 三路汇合到同一 schema 解析
}

std::expected<MCPClient*, std::string> MCPConnectionManager::client_for(
    McpServerInfo const& info) {
  auto it = clients_.find(info.name);
  if (it != clients_.end()) return it->second.get();
  auto client = std::make_unique<MCPClient>(
      *transport_, info.url,
      /*headers=*/[&] {
        std::vector<std::string> h;
        // 凭证注入：meta.token → Bearer（MCP 常用）；其它 meta 键作为自定义头（key: value）。
        if (info.meta.is_object()) {
          auto const tok = info.meta.find("token");
          if (tok != info.meta.end() && tok->is_string() && !tok->get<std::string>().empty())
            h.push_back("Authorization: Bearer " + tok->get<std::string>());
          for (auto const& [k, v] : info.meta.items())
            if (k != "token" && v.is_string() && !v.get<std::string>().empty())
              h.push_back(k + ": " + v.get<std::string>());
        }
        return h;
      }(),
      /*server_name=*/info.name);
  // 握手：连接状态机推进到 Initialized（失败 → 池不保留，整体报错）。
  auto handshake = client->initialize();
  if (!handshake.ok())
    return std::unexpected("MCP '" + info.name + "' 握手失败：" + handshake.error);
  auto* ptr = client.get();
  clients_[info.name] = std::move(client);
  return ptr;
}

std::vector<std::string> MCPConnectionManager::server_names() const {
  std::vector<std::string> out;
  out.reserve(clients_.size());
  for (auto const& [name, _] : clients_) out.push_back(name);
  return out;
}

}  // namespace cppchain::mcp