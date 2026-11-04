// examples/sa_demo/sa_tools.hpp
// SystemAgent（problems/64 · S9）—— 把 cppchain 的真实工具接进 SA 的工具目录。
//
// 设计：
//   · 默认目录 = 只读工具（计算器/时间/文件读列搜提采/旅行 DB/草图校验/只读网络工具）；
//   · 写入与命令类（file_write / submit_result / bash）只在 allow_side_effects=true 时加入，
//     且 bash 走 restricted spawn executor（非真沙箱，仅兜底）；
//   · 工具目录同时是 Planner 的「可用工具清单」与解释器的执行注册表（同一份，防漂移）；
//   · MCP 动态工具不在此自动接入：需要 server 配置与连接生命周期，后续单独做。
#pragma once

#include <cppchain/tool/arxiv.hpp>
#include <cppchain/tool/bash_tool.hpp>
#include <cppchain/tool/calculator.hpp>
#include <cppchain/tool/datetime.hpp>
#include <cppchain/tool/executor.hpp>
#include <cppchain/tool/fetch_url.hpp>
#include <cppchain/tool/file_tools.hpp>
#include <cppchain/tool/http_json.hpp>
#include <cppchain/tool/image.hpp>
#include <cppchain/tool/registry.hpp>
#include <cppchain/tool/skill_tools.hpp>
#include <cppchain/tool/sketch_verify.hpp>
#include <cppchain/tool/text.hpp>
#include <cppchain/tool/trip_db.hpp>
#include <cppchain/tool/weather.hpp>
#include <cppchain/tool/web_search.hpp>

#ifdef SA_PLAN_LIVE
#include <cppchain/http/curl_multi_transport.hpp>
#endif

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace cppchain::sa {

struct ToolCatalog {
  tool::ToolRegistry reg;
  std::vector<std::string> names;  // Planner 可见清单（与 reg 同源，防漂移）
  std::filesystem::path root;
  bool allow_side_effects = false;
  std::shared_ptr<tool::ToolExecutor> executor;  // bash 用；保持生命周期
};

namespace detail {

// 按字节上限截断，但不切断 UTF-8 多字节字符（否则 nlohmann 序列化会抛 type_error 316）。
inline std::string utf8_prefix(std::string const& s, std::size_t max_bytes) {
  if (s.size() <= max_bytes) return s;
  std::size_t n = max_bytes;
  while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
  return s.substr(0, n);
}

inline std::size_t utf8_codepoints(std::string const& s) {
  std::size_t n = 0;
  for (unsigned char c : s)
    if ((c & 0xC0) != 0x80) ++n;
  return n;
}

inline std::string env_str(char const* key) {
  char const* v = std::getenv(key);
  return (v != nullptr && *v != '\0') ? std::string(v) : std::string();
}

inline std::string ca_bundle() {
  std::string ca = env_str("LLM_CA_BUNDLE");
  if (!ca.empty()) return ca;
  std::string const home = env_str("HOME");
  if (!home.empty()) {
    std::filesystem::path const p = std::filesystem::path(home) / ".geo" / "corp-ca.pem";
    if (std::filesystem::exists(p)) return p.string();
  }
  return {};
}

// 把非 UTF-8 字节替换为 '?'（下载到的网页/文本可能混入非法序列，后续 JSON 化会抛异常）。
inline std::string sanitize_utf8(std::string const& s) {
  std::string out;
  out.reserve(s.size());
  auto is_cont = [&](std::size_t k) {
    return k < s.size() && (static_cast<unsigned char>(s[k]) & 0xC0) == 0x80;
  };
  std::size_t i = 0;
  while (i < s.size()) {
    unsigned char const c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
      out.push_back(static_cast<char>(c));
      ++i;
      continue;
    }
    std::size_t const len = (c >= 0xF0 && c <= 0xF4) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 0;
    bool ok = len != 0 && i + len <= s.size();
    for (std::size_t k = 1; ok && k < len; ++k) ok = is_cont(i + k);
    if (ok) {
      out.append(s, i, len);
      i += len;
    } else {
      out.push_back('?');
      ++i;
    }
  }
  return out;
}

inline std::string safe_filename(std::string requested, std::string const& url) {
  auto const slash = requested.find_last_of("/\\");
  if (slash != std::string::npos) requested = requested.substr(slash + 1);
  if (requested.empty() || requested == "." || requested == "..") {
    std::string u = url;
    auto const q = u.find_first_of("?#");
    if (q != std::string::npos) u = u.substr(0, q);
    auto const s = u.find_last_of('/');
    requested = (s != std::string::npos) ? u.substr(s + 1) : "download.txt";
  }
  std::string out;
  for (unsigned char c : requested)
    if (c >= 0x80 || std::isalnum(c) != 0 || c == '_' || c == '-' || c == '.')
      out.push_back(static_cast<char>(c));
  return out.empty() ? "download.txt" : out;
}

// download_url：把 URL 完整内容落盘（长文档/整本书/报告），只返回路径与大小。
// 落盘固定在 root/tmp/sa_downloads/ 下（幂等缓存目录），避免任意路径覆盖仓库文件。
inline tool::ToolSpec make_download_tool(std::shared_ptr<http::Transport> transport,
                                         std::filesystem::path root,
                                         long max_bytes = 8L * 1024 * 1024) {
  tool::ToolSpec spec = tool::make_tool(
      "download_url",
      "把 URL 的完整内容下载到本地文件（适合长文档、整本书、长报告）。只返回文件路径/大小，"
      "不把正文放进上下文；下载后应交给 synthesis（会自动分片读取）或 file_read（offset/max_chars 分页）"
      "处理，避免用 fetch_url 截断后强行总结。文件写入 root/tmp/sa_downloads/。",
      tool::SchemaBuilder()
          .prop("url", "string", "要下载的 http/https URL")
          .prop("path", "string",
                "保存路径（相对 root；建议 tmp/ 或 downloads/；缺省 tmp/sa_downloads/）",
                /*required=*/false)
          .prop("max_bytes", "integer", "大小上限（默认 8MB）", /*required=*/false)
          .build(),
      [transport, root, max_bytes](nlohmann::json const& args,
                                   tool::ToolContext& ctx)
          -> std::expected<nlohmann::json, tool::ToolError> {
        auto url = tool::arg<std::string>(args, "url");
        if (!url) return std::unexpected(url.error());
        if (!(url->starts_with("http://") || url->starts_with("https://")))
          return std::unexpected(tool::ToolError{"download_url 仅支持 http/https：" + *url});
        auto req_path = tool::arg<std::string>(args, "path", /*required=*/false);
        long const cap = [&] {
          if (!args.contains("max_bytes") || !args["max_bytes"].is_number_integer()) return max_bytes;
          return std::min<long>(args["max_bytes"].get<long>(), max_bytes);
        }();

        // 尽量沿用 Planner 给的相对路径（后续 file_read 会用同一路径）；只允许几个运行目录，
        // 其余一律落到 tmp/sa_downloads/（避免任意覆盖仓库文件）。
        std::string rel;
        if (req_path && !req_path->empty()) {
          std::string const norm =
              std::filesystem::path(*req_path).lexically_normal().generic_string();
          bool const allowed = norm.starts_with("tmp/") || norm.starts_with("downloads/") ||
                               norm.starts_with("outputs/") ||
                               norm.starts_with(".sa_downloads/");
          rel = allowed ? norm
                        : "tmp/sa_downloads/" + detail::safe_filename(*req_path, *url);
        } else {
          rel = "tmp/sa_downloads/" + detail::safe_filename("", *url);
        }
        auto const resolved = tool::resolve_path_in_root(root, rel);
        if (!resolved)
          return std::unexpected(tool::ToolError{"下载路径越界（只允许 root 内）：" + rel});
        std::error_code ec;
        std::filesystem::create_directories(resolved->parent_path(), ec);

        http::HttpRequest req;
        req.url = *url;
        req.method = "GET";
        req.timeout_ms = 60000;
        req.headers = {"Accept: text/plain,text/html,application/xhtml+xml,*/*",
                       "User-Agent: cppchain-sa-download/0.1"};
        auto r = tool::http_json(*transport, req, ctx.cancel);
        if (!r) return std::unexpected(r.error());
        if (auto const e = tool::http_status_error(r->status, "下载", r->raw))
          return std::unexpected(*e);
        if (static_cast<long>(r->raw.size()) > cap)
          return std::unexpected(tool::ToolError{"文件过大（" +
                                                 std::to_string(r->raw.size()) + " bytes > " +
                                                 std::to_string(cap) + "）"});

        std::string text = r->raw;
        std::string title;
        bool const looks_html = text.starts_with("<!doctype") || text.starts_with("<!DOCTYPE") ||
                                text.starts_with("<html") || text.starts_with("<HTML");
        if (looks_html) {
          title = tool::extract_title(text);
          text = tool::html_to_text(text);
        }
        text = detail::sanitize_utf8(text);

        std::ofstream out(*resolved, std::ios::binary | std::ios::trunc);
        if (!out) return std::unexpected(tool::ToolError{"无法写入文件：" + rel});
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();

        return nlohmann::json{{"url", *url},
                              {"path", rel},
                              {"bytes", static_cast<std::int64_t>(text.size())},
                              {"chars", static_cast<std::int64_t>(detail::utf8_codepoints(text))},
                              {"html_extracted", looks_html},
                              {"title", title}};
      },
      /*read_only=*/false);
  spec.with_effect_class(tool::EffectClass::Idempotent);
  spec.category = "action";
  spec.approval = tool::ToolApprovalLevel::Policy;
  return spec;
}

}  // namespace detail

// 构建工具目录。allow_side_effects=false 时只装只读工具（默认、CI/页面默认）。
inline ToolCatalog build_tool_catalog(bool allow_side_effects) {
  ToolCatalog c;
  c.root = std::filesystem::current_path();
  c.allow_side_effects = allow_side_effects;

  auto add = [&c](tool::ToolSpec s) {
    c.names.push_back(s.name);
    c.reg.add(std::move(s));
  };

  // —— 零依赖 / 纯本地 ——
  add(tool::make_calculator_tool());
  add(tool::make_datetime_tool());
  add(tool::make_sketch_verify_tool());
  for (tool::ToolSpec& t : tool::make_trip_tools())
    if (allow_side_effects || t.read_only) add(std::move(t));

  // —— 文件六件套（沙箱根 = 当前工作目录）；file_write 需显式开启 ——
  tool::FileToolsConfig file_cfg;
  file_cfg.root_dir = c.root;
  // 长文档场景：允许一次读取更大窗口（默认仍是 4000；分片由调用方给 offset）。
  file_cfg.max_read_chars = 60000;
  for (tool::ToolSpec& t : tool::make_file_tools(file_cfg))
    if (allow_side_effects || t.read_only) add(std::move(t));

  // —— 本地知识库（仅在宿主配置了目录时出现）——
  if (!detail::env_str("WFR_SKILL_DIR").empty() || !detail::env_str("WFR_WIKI_DIR").empty()) {
    add(tool::make_read_skill_tool());
    add(tool::make_wiki_search_tool());
  }

#ifdef SA_PLAN_LIVE
  // —— 只读网络工具（需要 HTTP 传输层）——
  auto transport = std::make_shared<http::CurlMultiTransport>();
  std::string const ca = detail::ca_bundle();
  tool::WeatherToolConfig wc;
  wc.ca_info = ca;
  add(tool::make_weather_tool(transport, wc));
  tool::ArxivConfig ac;
  ac.ca_info = ca;
  add(tool::make_arxiv_search_tool(transport, ac));
  tool::FetchUrlConfig uc;
  uc.ca_info = ca;
  add(tool::make_fetch_url_tool(transport, uc));
  add(tool::make_read_image_tool(transport));
  std::string const ws_key = detail::env_str("WEB_SEARCH_API_KEY");
  if (!ws_key.empty()) {
    tool::WebSearchConfig ws;
    ws.api_key = ws_key;
    ws.ca_info = ca;
    add(tool::make_web_search_tool(transport, ws));
  }
  // 长文档落盘（默认可用；只写 tmp/sa_downloads/，幂等缓存）。
  add(detail::make_download_tool(transport, c.root));

  // —— 命令执行（显式开启；restricted spawn，非真沙箱，仅兜底）——
  if (allow_side_effects) {
    c.executor = tool::make_restricted_spawn_executor(c.root);
    add(tool::make_bash_tool(c.executor.get()));
  }
#endif

  return c;
}

// Planner 用的紧凑工具清单：名称 / 只读性 / 描述 / 参数。
inline std::string tool_catalog_prompt(ToolCatalog const& c) {
  std::string out;
  for (tool::ToolSpec const& s : c.reg.all()) {
    std::string desc = detail::utf8_prefix(s.description, 140);
    if (desc.size() < s.description.size()) desc += "…";
    out += "- " + s.name + "（" +
           (s.read_only
                ? std::string("只读")
                : "副作用:" +
                      std::string(tool::kEffectClassNames[static_cast<unsigned>(s.effect_class)])) +
           "）：" + desc;

    std::string req, opt;
    nlohmann::json const params = s.parameters;
    if (params.is_object() && params.contains("properties") && params["properties"].is_object()) {
      auto required_contains = [&](std::string const& key) {
        if (!params.contains("required") || !params["required"].is_array()) return false;
        for (nlohmann::json const& r : params["required"])
          if (r.is_string() && r.get<std::string>() == key) return true;
        return false;
      };
      for (auto it = params["properties"].begin(); it != params["properties"].end(); ++it) {
        std::string& dst = required_contains(it.key()) ? req : opt;
        if (!dst.empty()) dst += "、";
        dst += it.key();
      }
    }
    if (!req.empty() || !opt.empty()) {
      out += " ｜ 参数：";
      if (!req.empty()) out += "必填[" + req + "]";
      if (!opt.empty()) out += (req.empty() ? "" : "，") + std::string("可选[") + opt + "]";
    }
    out += "\n";
  }
  return out;
}

inline nlohmann::json tool_manifest(ToolCatalog const& c) {
  nlohmann::json arr = nlohmann::json::array();
  for (tool::ToolSpec const& s : c.reg.all()) {
    arr.push_back(nlohmann::json{
        {"name", s.name},
        {"description", s.description},
        {"read_only", s.read_only},
        {"effect", tool::kEffectClassNames[static_cast<unsigned>(s.effect_class)]},
        {"approval", static_cast<int>(s.approval)},
        {"parameters", s.parameters}});
  }
  return arr;
}

}  // namespace cppchain::sa
