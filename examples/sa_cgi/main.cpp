// sa_cgi —— SA 运行 CGI（problems/64 · S5）：从浏览器触发 SA demo 运行。
//
// 协议：GET /cgi-bin/sa_cgi?scenario=all[&pace=300]  → application/json 运行回执
//       GET /cgi-bin/sa_cgi?planner=llm&query=<一句话>[&pace=300]
//   v1 仅支持 scenario=all（整包确定性重放 t1/t2 两任务，覆盖 tasklog）；
//   t1/t2 单场景触发 = v2（需把 S2/S4 导演段抽成可单独调用）。
//   S7：query 非空 → 一句话交给 sa_demo 的真机 LLM 规划器生成 DAG → 校验 → 执行；
//       query 经环境变量 SA_QUERY 传子进程（不拼 shell，零注入面）；URL 解码在此完成。
//   pace = 每条实时进度消息后的 wall-clock 停顿（ms，0..5000）；页面轮询
//   web/sa/sa_progress.jsonl 即可看到节点点亮/消息逐条出现的执行过程。
//   注：页面「源码」标签展示的是**当前任务 DAG 的定义**（页面内 sa-tasklog 数据，纯前端），
//   不经本端点；本端点只负责"触发运行 + 回执 + run_meta"。
//
// 职责链：解析 QUERY_STRING → 定位 sa_demo 二进制（/proc/self/exe 真实路径同目录，
//   或 SA_DEMO_BIN env 覆盖）→ chdir 到仓库根 → popen 子进程（stdout/stderr 重定向
//   web/sa/run.log）→ 测墙钟 → 更新 web/sa/run_meta.json（runs 追加、保留最近 50 条；
//   gitignored，纯宿主观测，不进 tasklog）→ 输出 JSON。
//
// 注意（诚实边界）：sa_cgi 只是"触发 + 回执 + meta"，不决定执行期语义；
//   全部执行逻辑在 sa_demo（全离线确定性，R5 纪律：不碰真模型）。
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

using Json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::string env_or(char const* name, std::string const& dflt) {
  char const* v = std::getenv(name);
  return (v != nullptr && *v != '\0' ? std::string(v) : dflt);
}

// CGI 本体所在目录（build/dev/examples）。web/cgi-bin/sa_cgi 是 symlink，
// /proc/self/exe 解析到真实二进制，再取父目录。
fs::path cgi_bin_dir() {
  std::string const realexe = env_or("SA_CGI_REALEXE", "");
  if (!realexe.empty()) return fs::weakly_canonical(fs::path(realexe)).parent_path();
  std::error_code ec;
  fs::path const real = fs::read_symlink("/proc/self/exe", ec);
  if (ec || real.empty()) return fs::current_path();
  return fs::weakly_canonical(real).parent_path();
}

// URL 解码（S7 一句话查询含中文，必须解码；+ → 空格）。
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

// QUERY_STRING 极简解析（键名/值按 & 与 = 切分，不做 URL 解码）。
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

}  // namespace

int main() {
  std::string const scenario = query_param("scenario");
  std::string const planner = query_param("planner");
  std::string query = url_decode(query_param("query"));
  // 一句话规划（S7）：query 非空即进入；长度上限防滥用；NUL 直接拒（会截断 env）。
  bool const plan_mode = !query.empty();
  Json out;
  out["schema"] = "sa-run-cgi/v1";
  out["scenario"] = scenario.empty() ? "all" : scenario;
  out["mode"] = plan_mode ? "plan" : "scripted";
  if (plan_mode) out["query"] = query;

  // CGI 首行是响应头；fail 也输出 JSON（页面靠 ok 字段判断，不靠 HTTP 码）。
  auto respond = [&out] {
    std::print("Content-Type: application/json\r\n\r\n");
    std::println("{}", out.dump());
    return 0;
  };

  if (plan_mode) {
    if (query.size() > 800) {
      out["ok"] = false;
      out["error"] = "query 过长（上限 800 字节）";
      return respond();
    }
    if (query.find('\0') != std::string::npos) {
      out["ok"] = false;
      out["error"] = "query 含非法字符";
      return respond();
    }
  } else if (planner == "llm") {
    out["ok"] = false;
    out["error"] = "planner=llm 需要同时提供 query（一句话）";
    return respond();
  }

  if (!scenario.empty() && scenario != "all") {
    out["ok"] = false;
    out["error"] = "scenario 仅支持 all（t1/t2 单场景触发为 v2，依赖导演段抽离）";
    return respond();
  }

  fs::path const bindir = cgi_bin_dir();
  // bindir = <root>/build/dev/examples → 父级 ×3 = 仓库根
  fs::path const root = bindir.parent_path().parent_path().parent_path();

  // 数据目录：SA_DATA_DIR（隔离/测试用，缺省 SA_DATA_DIR 时 = <root>/web/sa）。
  // E2E 既可指向临时目录，也可用真实目录配备份恢复。
  std::error_code mk_ec;
  fs::path data_dir = root / "web" / "sa";
  std::string const dd = env_or("SA_DATA_DIR", "");
  if (!dd.empty()) data_dir = fs::path(dd);
  fs::create_directories(data_dir, mk_ec);

  // S8：删除历史任务（持久删除：从 sa_tasklog.json 移除并原子落盘）。
  // 只接受任务 id（白名单字符），不拼路径；不需要 sa_demo 二进制。
  if (std::string const action = query_param("action"); action == "delete") {
    std::string const id = url_decode(query_param("id"));
    bool ok_id = !id.empty() && id.size() <= 64;
    for (char c : id)
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.'))
        ok_id = false;
    if (!ok_id) {
      out["ok"] = false;
      out["error"] = "非法任务 id";
      return respond();
    }
    fs::path const tl = data_dir / "sa_tasklog.json";
    Json j;
    try {
      std::ifstream in(tl);
      j = Json::parse(in);
    } catch (...) {
      out["ok"] = false;
      out["error"] = "历史库不存在或不可解析：" + tl.string();
      return respond();
    }
    if (!j.contains("tasks") || !j["tasks"].is_array()) {
      out["ok"] = false;
      out["error"] = "历史库缺 tasks 数组";
      return respond();
    }
    Json kept = Json::array();
    bool found = false;
    for (Json const& t : j["tasks"]) {
      if (t.value("id", "") == id)
        found = true;
      else
        kept.push_back(t);
    }
    if (!found) {
      out["ok"] = false;
      out["error"] = "任务不存在：" + id;
      return respond();
    }
    j["tasks"] = std::move(kept);
    fs::path const tmp = tl;
    std::string const tmp_path = tmp.string() + ".tmp";
    {
      std::ofstream o(tmp_path, std::ios::trunc);
      if (!o) {
        out["ok"] = false;
        out["error"] = "写历史库失败";
        return respond();
      }
      o << j.dump(2) << "\n";
    }
    std::error_code ren_ec;
    fs::rename(tmp_path, tl, ren_ec);
    if (ren_ec) {
      std::ofstream o(tl, std::ios::trunc);
      o << j.dump(2) << "\n";
      std::error_code rm_ec;
      fs::remove(tmp_path, rm_ec);
    }
    out["ok"] = true;
    out["deleted"] = id;
    out["remaining"] = static_cast<std::int64_t>(j["tasks"].size());
    return respond();
  }

  fs::path sa_demo = fs::path(env_or("SA_DEMO_BIN", ""));
  if (sa_demo.empty()) sa_demo = bindir / "sa_demo";
  if (!fs::exists(sa_demo)) {
    out["ok"] = false;
    out["error"] = "找不到 sa_demo 二进制：" + sa_demo.string() + "（先 ./run-demo.sh 构建）";
    return respond();
  }

  std::error_code ec;
  fs::current_path(root, ec);
  if (ec) {
    out["ok"] = false;
    out["error"] = "chdir 仓库根失败：" + ec.message();
    return respond();
  }

  fs::path const tl = data_dir / "sa_tasklog.json";
  fs::path const log = data_dir / "run.log";
  fs::path const prog = data_dir / "sa_progress.jsonl";

  // S6c：pace 参数（毫秒，按每条进度消息放慢；缺省 0 = 全速，仅页面运行传非 0）。
  // 只接受纯数字，避免把用户输入拼进 shell 命令。
  long pace_ms = 0;
  {
    std::string const p = query_param("pace");
    bool digits = !p.empty() && p.size() <= 5;
    for (char c : p)
      if (c < '0' || c > '9') digits = false;
    if (digits) pace_ms = std::stol(p);
    if (pace_ms > 5000) pace_ms = 5000;
  }
  // 单运行锁（S8）：两个标签页同时点运行时，后到者明确失败，避免 tasklog/进度互相覆盖。
  // flock 随进程退出自动释放；open 失败则退化为不锁（demo 可用性优先）。
  if (int const lock_fd = ::open((data_dir / ".run.lock").c_str(), O_CREAT | O_RDWR, 0600);
      lock_fd >= 0 && ::flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
    ::close(lock_fd);
    out["ok"] = false;
    out["error"] = "已有一次运行在进行，请等它结束再试";
    return respond();
  }

  // 先清空进度文件：页面可能在 spawn 前就开始轮询，避免读到上一次运行的残留事件。
  { std::ofstream clear(prog, std::ios::trunc); }

  // S7：一句话经**环境变量**传给子进程（不拼进 shell 命令，零注入面）。
  if (plan_mode)
    ::setenv("SA_QUERY", query.c_str(), 1);
  else
    ::unsetenv("SA_QUERY");

  auto const t0 = std::chrono::steady_clock::now();
  std::string const cmd = sa_demo.string() + " --tasklog=" + tl.string() +
                           " --progress=" + prog.string() +
                           " --pace-ms=" + std::to_string(pace_ms) + " > " + log.string() +
                           " 2>&1";
  bool spawned = false;
  int rc = 1;
  if (std::FILE* p = popen(cmd.c_str(), "r")) {
    spawned = true;
    char buf[256];
    while (std::fgets(buf, sizeof buf, p) != nullptr) { /* drain：输出已重定向 run.log，管道近空 */ }
    rc = pclose(p);
  }
  auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();

  if (!spawned) {
    out["ok"] = false;
    out["error"] = "popen 失败：无法启动 " + sa_demo.string();
    return respond();
  }
  if (rc != 0) {
    // 规划模式的 rc=3 = 世界断言未过：tasklog 仍然写出（页面应看到 Failed 的任务）。
    bool const plan_failed_with_task = plan_mode && rc == 3 && fs::exists(tl);
    if (!plan_failed_with_task) {
      // 把 run.log 末几行带回页面（规划失败/校验不过的原因在 stdout 里）。
      std::string tail;
      if (std::ifstream in(log); in) {
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(in, line)) {
          lines.push_back(line);
          if (lines.size() > 8) lines.erase(lines.begin());
        }
        for (std::string const& s : lines) {
          if (!tail.empty()) tail += " | ";
          tail += s;
        }
        if (tail.size() > 600) tail = tail.substr(tail.size() - 600);
      }
      out["ok"] = false;
      out["error"] = "sa_demo 退出码 " + std::to_string(rc) +
                     (tail.empty() ? "（详见 " + log.string() + "）" : "：" + tail);
      return respond();
    }
    out["task_failed"] = true;
  }
  if (!fs::exists(tl)) {
    out["ok"] = false;
    out["error"] = "运行完成但未产出 " + tl.string();
    return respond();
  }

  // 读回本次运行产生的任务（tasklog 是唯一真相）。
  // S8 历史库：数组是「最近优先」——规划模式的新任务在最前，只取第一条；
  // 脚本模式只回传本轮稳定 id t1/t2（历史里的其他任务不算本次产物）。
  std::vector<std::string> task_ids;
  try {
    std::ifstream in(tl);
    Json const tl_json = Json::parse(in);
    Json const tasks = tl_json.value("tasks", Json::array());
    if (plan_mode) {
      if (!tasks.empty()) {
        Json const& t = tasks.front();
        task_ids.push_back(t.value("id", "?"));
        out["title"] = t.value("title", "");
        out["task_status"] = t.value("status", "");
        Json const dag = t.value("dag", Json::object());
        out["dag_nodes"] = static_cast<std::int64_t>(dag.value("nodes", Json::array()).size());
        out["dag_edges"] = static_cast<std::int64_t>(dag.value("edges", Json::array()).size());
      }
    } else {
      for (Json const& t : tasks) {
        std::string const id = t.value("id", "?");
        if (id == "t1" || id == "t2") task_ids.push_back(id);
      }
    }
  } catch (...) { /* 列表拿不到不强判失败 */ }

  // run_meta 追加（宿主观测：墙钟/次数；gitignored，不属于 tasklog 语义）。
  auto const now_s = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  Json meta = Json{{"runs", Json::array()}};
  try {
    std::ifstream in(data_dir / "run_meta.json");
    Json old = Json::parse(in);
    if (old.contains("runs") && old["runs"].is_array()) meta = old;
  } catch (...) { /* 首次建立 */ }
  Json entry = Json{{"at", now_s}, {"scenario", scenario.empty() ? "all" : scenario},
                    {"ms", ms},    {"tasks", Json::array()}};
  for (std::string const& tid : task_ids) entry["tasks"].push_back(tid);
  meta["runs"].push_back(entry);
  if (meta["runs"].size() > 50) {
    Json kept = Json::array();
    std::size_t const n = meta["runs"].size();
    for (std::size_t i = n - 50; i < n; ++i) kept.push_back(meta["runs"][i]);
    meta["runs"] = std::move(kept);
  }
  meta["total"] = static_cast<std::int64_t>(meta["runs"].size());
  {
    std::ofstream o(data_dir / "run_meta.json");
    o << meta.dump(2) << "\n";
  }

  out["ok"] = true;
  out["elapsed_ms"] = ms;
  out["run_total"] = meta["total"];
  out["last_run_at"] = now_s;
  out["task_ids"] = Json::array();
  for (std::string const& tid : task_ids) out["task_ids"].push_back(tid);
  out["tasklog"] = tl.string();
  out["pace_ms"] = pace_ms;
  out["progress"] = prog.string();

  return respond();
}