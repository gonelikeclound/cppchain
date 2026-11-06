// sad_cgi —— SAD 编排台 CGI（problems/64 S10）
//
// 协议（GET /cgi-bin/sad_cgi?action=...）：
//   action=scenario（默认）：跑全量 mock 场景（984 事件）→ 返回报告 + 追加处理历史
//   action=simulate&event=<URL 编码 JSON>：单事件模拟 → 返回 decision/dag/outputs + 追加历史
//   action=history：返回处理历史
//   action=clear-history：清空历史
//
// 职责：只做"触发/读取/历史持久化"，执行逻辑全部在 sad_mock（纯离线）。
// 事件 JSON 经临时文件传给子进程（不拼 shell，零注入面）；历史原子写。
#include <nlohmann/json.hpp>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <vector>

using Json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::string env_or(char const* name, std::string const& dflt) {
  char const* v = std::getenv(name);
  return (v != nullptr && *v != '\0') ? std::string(v) : dflt;
}

fs::path self_dir() {
  std::error_code ec;
  fs::path const real = fs::read_symlink("/proc/self/exe", ec);
  if (ec || real.empty()) return fs::current_path();
  return fs::weakly_canonical(real).parent_path();
}

// QUERY_STRING 解析（键值按 & 与 = 切分，不做 URL 解码）。
std::string query_param(char const* name) {
  char const* qs = std::getenv("QUERY_STRING");
  if (qs == nullptr) return {};
  std::string const q(qs);
  std::size_t pos = 0;
  while (pos <= q.size()) {
    std::size_t const amp = q.find('&', pos);
    std::string const kv =
        q.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
    std::size_t const eq = kv.find('=');
    std::string const k = eq == std::string::npos ? kv : kv.substr(0, eq);
    std::string const v = eq == std::string::npos ? std::string() : kv.substr(eq + 1);
    if (k == name) return v;
    if (amp == std::string::npos) break;
    pos = amp + 1;
  }
  return {};
}

std::string url_decode(std::string const& s) {
  auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '+') {
      out.push_back(' ');
    } else if (s[i] == '%' && i + 2 < s.size()) {
      int const hi = hex(s[i + 1]);
      int const lo = hex(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>(hi * 16 + lo));
        i += 2;
        continue;
      }
      out.push_back(s[i]);
    } else {
      out.push_back(s[i]);
    }
  }
  return out;
}

bool write_atomic(fs::path const& p, Json const& j) {
  fs::path const tmp = p.string() + ".tmp";
  {
    std::ofstream o(tmp, std::ios::trunc);
    if (!o) return false;
    o << j.dump(2) << "\n";
  }
  std::error_code ec;
  fs::rename(tmp, p, ec);
  if (ec) {
    std::ofstream o(p, std::ios::trunc);
    if (!o) return false;
    o << j.dump(2) << "\n";
    std::error_code ec2;
    fs::remove(tmp, ec2);
  }
  return true;
}

Json load_history(fs::path const& p) {
  std::ifstream in(p);
  if (!in) return Json{{"entries", Json::array()}};
  Json j = Json::parse(in, nullptr, false);
  if (j.is_discarded() || !j.is_object() || !j.contains("entries") ||
      !j["entries"].is_array())
    return Json{{"entries", Json::array()}};
  return j;
}

void append_history(fs::path const& p, Json entry) {
  Json h = load_history(p);
  h["entries"].push_back(std::move(entry));
  while (h["entries"].size() > 200) h["entries"].erase(h["entries"].begin());
  write_atomic(p, h);
}

long long now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// 运行 sad_mock；capture=true 时返回 stdout（simulate 模式），否则把输出重定向到 run.log。
std::string run_sad(fs::path const& sad, std::string const& args, fs::path const& log,
                    bool capture) {
  std::string const cmd = sad.string() + " " + args +
                          (capture ? " 2>/dev/null" : " > " + log.string() + " 2>&1");
  std::string out;
  if (std::FILE* p = popen(cmd.c_str(), "r")) {
    char buf[4096];
    while (std::fgets(buf, sizeof buf, p) != nullptr)
      if (capture) out += buf;
    pclose(p);
  }
  return out;
}

}  // namespace

int main() {
  std::string const action = query_param("action");
  Json out;
  out["schema"] = "sad-cgi/v1";
  out["action"] = action.empty() ? "scenario" : action;

  auto respond = [&out] {
    std::print("Content-Type: application/json\r\n\r\n");
    std::println("{}", out.dump());
    return 0;
  };

  fs::path const bindir = self_dir();
  fs::path const root = bindir.parent_path().parent_path().parent_path();
  fs::path data_dir = root / "web" / "sad";
  std::string const dd = env_or("SAD_DATA_DIR", "");
  if (!dd.empty()) data_dir = fs::path(dd);
  std::error_code ec;
  fs::create_directories(data_dir, ec);

  fs::path sad = fs::path(env_or("SAD_MOCK_BIN", ""));
  if (sad.empty()) sad = bindir / "sad_mock";
  if (!fs::exists(sad)) {
    out["ok"] = false;
    out["error"] = "找不到 sad_mock 二进制：" + sad.string();
    return respond();
  }

  fs::path const history = data_dir / "sad_history.json";
  fs::path const log = data_dir / "run.log";

  if (action == "history") {
    Json const h = load_history(history);
    out["ok"] = true;
    out["entries"] = h["entries"];
    out["count"] = static_cast<std::int64_t>(h["entries"].size());
    return respond();
  }

  if (action == "clear-history") {
    Json const h = Json{{"entries", Json::array()}};
    out["ok"] = write_atomic(history, h);
    out["cleared"] = true;
    return respond();
  }

  if (action == "simulate") {
    std::string const raw = url_decode(query_param("event"));
    if (raw.empty() || raw.size() > 4000) {
      out["ok"] = false;
      out["error"] = "event 缺失或过长（上限 4000 字节）";
      return respond();
    }
    Json ev = Json::parse(raw, nullptr, false);
    if (ev.is_discarded() || !ev.is_object()) {
      out["ok"] = false;
      out["error"] = "event 不是合法 JSON 对象";
      return respond();
    }
    fs::path const ev_path = data_dir / ".sim_event.json";
    if (!write_atomic(ev_path, ev)) {
      out["ok"] = false;
      out["error"] = "写模拟事件失败";
      return respond();
    }
    std::string const text =
        run_sad(sad, "--simulate-event=" + ev_path.string(), log, /*capture=*/true);
    Json res = Json::parse(text, nullptr, false);
    if (res.is_discarded() || !res.is_object()) {
      out["ok"] = false;
      out["error"] = "sad_mock 模拟输出不可解析";
      return respond();
    }
    Json entry{{"id", "sim-" + std::to_string(now_ms())},
               {"at", std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count()},
               {"kind", "simulate"},
               {"ok", res.value("ok", false)},
               {"event", res.value("event", Json::object())},
               {"layer", res.value("layer", "")},
               {"decision", res.value("decision", "")},
               {"reason", res.value("reason", "")},
               {"task_id", res.value("task_id", "")},
               {"dag", res.value("spec", Json::object()).value("template", std::string())},
               {"spec", res.value("spec", Json::object())},
               {"trace_steps", res.value("trace_steps", Json::array())},
               {"outputs", res.value("outputs", Json::array())}};
    append_history(history, entry);
    out["ok"] = true;
    out["result"] = std::move(res);
    out["history_id"] = entry["id"];
    return respond();
  }

  // 默认：全量场景。页面首屏可传 cache=1&record=0：10 分钟内直接复用上次报告，
  // 避免每次刷新都触发 SAD 理解层的 LLM 调用。
  bool const record = query_param("record") != "0";
  bool const use_cache = query_param("cache") == "1";
  fs::path const report = data_dir / "sad_report.json";
  if (use_cache && fs::exists(report)) {
    try {
      std::ifstream in(report);
      Json cached = Json::parse(in);
      auto const now = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
      long long const gen = cached.value("generated_at", 0LL);
      if (gen > 0 && now - gen <= 600) {
        out["ok"] = true;
        out["report"] = std::move(cached);
        out["cache_hit"] = true;
        return respond();
      }
    } catch (...) { /* 缓存坏了就重跑 */ }
  }
  run_sad(sad, "--report=" + report.string(), log, /*capture=*/false);
  Json rep;
  try {
    std::ifstream in(report);
    rep = Json::parse(in);
  } catch (...) {
    out["ok"] = false;
    out["error"] = "场景报告读取失败：" + report.string();
    return respond();
  }
  Json entry{{"id", "scn-" + std::to_string(now_ms())},
             {"at", std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count()},
             {"kind", "scenario"},
             {"ok", rep.value("ok", false)},
             {"metrics", rep.value("metrics", Json::object())},
             {"sad_dag", rep.value("sad_dag", Json::object())},
             {"dags", rep.value("dags", Json::array())},
             {"tasks", rep.value("tasks", Json::array())},
             {"sad_traces", rep.value("sad_traces", Json::array())},
             {"checks", rep.value("checks", Json::array())}};
  if (record) append_history(history, entry);
  out["ok"] = true;
  out["report"] = std::move(rep);
  if (record) out["history_id"] = entry["id"];
  return respond();
}
