// examples/support/dotenv.hpp
// 极简 .env 读取（仅 demo 用，不进库）：KEY=VALUE，忽略空行与 # 注释。
// 优先级：真实环境变量 > .env 文件 > 默认值。密钥永远只在内存里流转。
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace demo {

inline std::string trim(std::string s) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

// 依次尝试候选路径，命中第一个存在的即返回
inline std::unordered_map<std::string, std::string> load_dotenv(
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
    break;  // 找到第一个可用文件即可
  }
  return out;
}

inline std::string env_or(std::unordered_map<std::string, std::string> const& dotenv,
                          std::string const& key, std::string const& fallback = {}) {
  if (char const* e = std::getenv(key.c_str()); e && *e) return e;
  if (auto it = dotenv.find(key); it != dotenv.end() && !it->second.empty()) return it->second;
  return fallback;
}

}  // namespace demo