// sa_cgi —— SA 运行 CGI（problems/64 · S5）：从浏览器触发 SA demo 运行。
//
// 协议：GET /cgi-bin/sa_cgi?scenario=all  → application/json 运行回执
//   v1 仅支持 scenario=all（整包确定性重放 t1/t2 两任务，覆盖 tasklog）；
//   t1/t2 单场景触发 = v2（需把 S2/S4 导演段抽成可单独调用）。
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

// QUERY_STRING 极简解析（scenario 值纯 ASCII，不做 URL 解码）。
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
  Json out;
  out["schema"] = "sa-run-cgi/v1";
  out["scenario"] = scenario.empty() ? "all" : scenario;

  // CGI 首行是响应头；fail 也输出 JSON（页面靠 ok 字段判断，不靠 HTTP 码）。
  auto respond = [&out] {
    std::print("Content-Type: application/json\r\n\r\n");
    std::println("{}", out.dump());
    return 0;
  };

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

  auto const t0 = std::chrono::steady_clock::now();
  std::string const cmd = sa_demo.string() + " --tasklog=" + tl.string() + " > " + log.string() +
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
    out["ok"] = false;
    out["error"] = "sa_demo 退出码 " + std::to_string(rc) + "（详见 " + log.string() + "）";
    return respond();
  }
  if (!fs::exists(tl)) {
    out["ok"] = false;
    out["error"] = "运行完成但未产出 " + tl.string();
    return respond();
  }

  // 读回任务 id 列表（tasklog 是唯一真相）。
  std::vector<std::string> task_ids;
  try {
    std::ifstream in(tl);
    Json tl_json = Json::parse(in);
    for (Json const& t : tl_json["tasks"]) task_ids.push_back(t.value("id", "?"));
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

  return respond();
}