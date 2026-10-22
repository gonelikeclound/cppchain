// cppchain —— plugins/qplugin.cpp
// 给 geo-server（Rust）的动态加载插件（C ABI，独立 .so，dlopen 引入）。
//   qp_revise(current_json, feedback, wfr_path, workdir, &err)
//     → 内部跑 `wfr question_revise --workdir <ws> --session question_revise.qp<pid>
//        --set current=<json> --set feedback=<text> <text>`，从会话日志取最后一条 AI
//        消息（= 完整新版题目 JSON）返回；失败返回 NULL 并置 err。
//   qp_free(ptr)          —— 释放 qp_revise 返回的字符串。
// 复用 cppchain：run_argv（子进程执行）+ replay_session_messages（读会话）。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <filesystem>
#include <string>

#include "cppchain/tool/spawn_runner.hpp"
#include "cppchain/workflow/session_log.hpp"

extern "C" {

static std::string session_log_path(std::string const& ws, std::string const& name) {
  return (std::filesystem::path(ws) / ".wfr" / "sessions" / ("session-" + name + ".log.jsonl"))
      .string();
}

// 返回最后一条 AI 消息的纯文本；无则返回空串。
static std::string last_ai_text(std::string const& ws, std::string const& session) {
  auto const msgs = cppchain::workflow::replay_session_messages(session_log_path(ws, session));
  for (auto it = msgs.rbegin(); it != msgs.rend(); ++it)
    if (it->type == cppchain::message::Role::Ai) return it->text();  // 便利方法：text 块拼接
  return "";
}

// 运行一次 question_revise，返回最终新版题目 JSON（调用方用 qp_free 释放）。
const char* qp_revise(const char* current, const char* feedback, const char* wfr_path,
                      const char* workdir, char** err) {
  if (err) *err = nullptr;
  if (!current || !feedback || !wfr_path || !workdir) {
    if (err) { std::string _e = "参数缺失（current/feedback/wfr_path/workdir 必须给）"; *err = ::strdup(_e.c_str()); }
    return nullptr;
  }
  std::string const ws = workdir;
  std::string const session = "question_revise.qp" + std::to_string(::getpid());
  std::string const wfr = wfr_path;

  std::vector<std::string> av = {
      wfr, "question_revise", "--workdir", ws, "--session", session,
      "--set", std::string("current=") + current,
      "--set", std::string("feedback=") + feedback,
      feedback};
  auto const out = cppchain::tool::detail::run_argv(
      av, /*cwd=*/std::nullopt, cppchain::tool::detail::SpawnLimits{},
      /*timeout=*/std::chrono::minutes(5), /*max_output=*/4096);
  if (out.exit_code != 0) {
    if (err) { std::string _e = "wfr 退出码 " + std::to_string(out.exit_code) + "：" + out.note; *err = ::strdup(_e.c_str()); }
    return nullptr;
  }
  std::string const answer = last_ai_text(ws, session);
  if (answer.empty()) {
    if (err) { std::string _e = "会话里没有 AI 回答"; *err = ::strdup(_e.c_str()); }
    return nullptr;
  }
  char* p = static_cast<char*>(std::malloc(answer.size() + 1));
  std::memcpy(p, answer.data(), answer.size());
  p[answer.size()] = '\0';
  return p;
}

void qp_free(const char* p) { std::free(const_cast<char*>(p)); }

const char* qp_version() { return "0.1.0"; }

}  // extern "C"
