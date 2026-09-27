// cppchain —— examples/proactive/ask.hpp
// ask_user 通用提问器：生成期澄清的"向用户提问"机制（仅 demo 用，不进库）。
// 两种取答方式：
//   · 交互 —— 有 options 打编号菜单（<问题>\n1. 选项1\n2. 选项2 …）等 stdin 选数字，
//     也可直接输文字；无 options 读自由文本。
//   · 批量 —— 按 --question-answer <问题正则> <序号|文本> 规则匹配问题文本自动作答；
//     未命中 → 有 options 取第 1 项，无 options 取默认文本 "默认"。
#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include <print>

#include "cppchain/workflow/ir.hpp"

#include "support/dotenv.hpp"  // demo::trim

namespace proactive {

class AnswerRouter {
 public:
  bool interactive = true;                                  // false → 批量（查规则）
  std::vector<std::pair<std::regex, std::string>> rules;    // 问题正则 → 答案值

  // 取一个问题（连带可选 2–4 个选项）的答案；返回 false 表示用户中断（Ctrl-D）。
  bool ask(std::string const& text, std::vector<std::string> const& options,
           std::string& out) {
    if (interactive) return ask_interactive(text, options, out);
    return ask_batch(text, options, out);
  }

 private:
  bool ask_interactive(std::string const& text,
                       std::vector<std::string> const& options, std::string& out) {
    std::println("  ask_user: {}", text);
    if (!options.empty()) {
      for (std::size_t i = 0; i < options.size(); ++i)
        std::println("    {}. {}", i + 1, options[i]);
      for (;;) {
        std::print("  选择数字（或直接输入文本）> ");
        std::fflush(stdout);
        std::string line;
        if (!std::getline(std::cin, line)) return false;  // Ctrl-D
        line = demo::trim(line);
        if (line.empty()) continue;
        char* end = nullptr;
        long const n = std::strtol(line.c_str(), &end, 10);
        if (end != line.c_str() && *end == '\0' && n >= 1 &&
            n <= static_cast<long>(options.size())) {
          out = options[static_cast<std::size_t>(n - 1)];
          return true;
        }
        out = line;  // 自由文本直接作答
        return true;
      }
    }
    std::print("  回答 > ");
    std::fflush(stdout);
    std::string line;
    if (!std::getline(std::cin, line)) return false;
    out = demo::trim(line);
    return true;
  }

  bool ask_batch(std::string const& text,
                 std::vector<std::string> const& options, std::string& out) {
    for (auto const& [re, val] : rules) {
      if (!std::regex_search(text, re)) continue;
      if (!options.empty()) {
        char* end = nullptr;
        long const n = std::strtol(val.c_str(), &end, 10);
        if (end != val.c_str() && *end == '\0') {  // 数字 → 选项序号（夹 1..n）
          long const clamped = std::clamp(n, 1L, static_cast<long>(options.size()));
          out = options[static_cast<std::size_t>(clamped - 1)];
        } else {
          out = val;  // 直接当文本答案
        }
      } else {
        out = val;
      }
      std::println("  ask_user: {}  [规则命中 → {}]", text, out);
      return true;
    }
    out = options.empty() ? std::string("默认") : options[0];
    std::println("  ask_user: {}  [未命中规则 → 默认 {}]", text, out);
    return true;
  }
};

}  // namespace proactive