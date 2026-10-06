// cppchain —— src/wfr/util.hpp
// wfr CLI 的宿主小工具（正式产出，不依赖 examples/support）：
// 极简 .env 读取（KEY=VALUE，忽略空行与 # 注释；优先级 真环境变量 > .env > 默认）
// 与 CA bundle 探测（企业代理环境：LLM_CA_BUNDLE 或 ~/.geo/corp-ca.pem）。
// 源于 examples/support/dotenv.hpp 的正式化拷贝（demo 版不进库，这里直接内联）。
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace cppchain::wfr {

[[nodiscard]] inline std::string trim(std::string s) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

// 依次尝试候选路径，命中第一个存在的即返回
[[nodiscard]] inline std::unordered_map<std::string, std::string> load_dotenv(
    std::vector<std::string> const& candidates) {
  std::unordered_map<std::string, std::string> out;
  for (auto const& path : candidates) {
    std::ifstream in(path);
    if (!in) continue;
    std::string line;
    while (std::getline(in, line)) {
      line = trim(line);
      if (line.empty() || line.front() == '#') continue;
      auto eq = line.find('=');
      if (eq == std::string::npos) continue;
      std::string key = trim(line.substr(0, eq));
      std::string val = trim(line.substr(eq + 1));
      if (val.size() >= 2 && (val.front() == '"' || val.front() == '\'') &&
          val.back() == val.front())
        val = val.substr(1, val.size() - 2);
      if (!key.empty()) out[key] = val;
    }
    break;
  }
  return out;
}

[[nodiscard]] inline std::string env_or(std::unordered_map<std::string, std::string> const& dotenv,
                                        std::string const& key,
                                        std::string const& fallback = {}) {
  if (char const* e = std::getenv(key.c_str()); e && *e) return e;
  if (auto it = dotenv.find(key); it != dotenv.end() && !it->second.empty()) return it->second;
  return fallback;
}

// 企业代理环境的 CA bundle：LLM_CA_BUNDLE 或 ~/.geo/corp-ca.pem（存在即用）
[[nodiscard]] inline std::string ca_bundle_from_env() {
  auto dotenv = load_dotenv({".env", "../.env", "../../.env"});
  std::string ca = env_or(dotenv, "LLM_CA_BUNDLE");
  if (ca.empty()) {
    if (char const* home = std::getenv("HOME")) {
      auto const geo = std::filesystem::path(home) / ".geo" / "corp-ca.pem";
      if (std::filesystem::exists(geo)) ca = geo.string();
    }
  }
  return ca;
}

}  // namespace cppchain::wfr