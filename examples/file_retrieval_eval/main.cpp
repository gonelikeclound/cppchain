// demo: file_retrieval_eval —— problems/02 追加「七法统一评测」（离线、确定性）
//
// 目的：用同一套语料/任务/指标，横向比较"取全 / 定位 / 预算"三轴上的手段：
//   ① 翻页读全 file_read(offset)      ② 落盘回读（在 S3 已由工作流消费，本台不重复）
//   ③ grep 字面定位                   ④ 结构化提取 file_extract
//   ⑦ 采样跳读 file_sample            混合路由（按问题形态选 1 法）
//   ⑤ BM25 / ⑥ 向量：见 longdoc 验收（problems/57·59·60，三问读入 13.83%），本台不重跑。
//
// 指标：hit（答案是否进入返回文本 = Read@budget 的确定性版）、calls（工具轮数）、
//       bytes（返回文本字节数，≈ tokens 的 1/5）。每个任务都预置"答案位置"，
//       因此不需要模型判分——**先验对错与模型行为分开**：本台测"方法能不能把答案取进上下文"。
#include <cppchain/tool/file_tools.hpp>

#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <vector>

using namespace cppchain;
using namespace cppchain::tool;
using nlohmann::json;

namespace {

namespace fs = std::filesystem;

struct Task {
  std::string id;
  std::string corpus;
  std::string answer;    // 判定命中：返回文本里出现该子串
  std::string needle;    // ③ grep 用（空 = 该方法不适用）
  std::string format;    // ④ extract 用（空 = 不适用）
  std::string selector;  // ④ extract 选择器
  std::string intent;    // 人类可读的问题形态
  std::string route;     // 混合路由的"正确选择"：extract / grep / sample / paging
};

struct Arm {
  bool applicable = false;
  bool hit = false;
  std::size_t calls = 0;
  std::size_t bytes = 0;
};

fs::path g_root;

void write_file(fs::path const& rel, std::string const& content) {
  fs::create_directories((g_root / rel).parent_path());
  std::ofstream out(g_root / rel, std::ios::binary | std::ios::trunc);
  out << content;
}

// ---- 语料：每份都埋"答案位置"，且足够大，让"整段读"代价可见 ----

void build_corpus() {
  fs::remove_all(g_root);
  fs::create_directories(g_root);

  // log.txt：4000 行，65% 处一条 ERROR（grep 的靶子），最后一行一个尾标记（翻页的靶子）
  {
    std::string s;
    for (int i = 0; i < 4000; ++i) {
      if (i == 2600)
        s += "2026-10-27T12:00:00Z ERROR code=0x9F3A handler=payment retry=3\n";
      else
        s += "2026-10-27T12:00:00Z INFO request id=" + std::to_string(100000 + i) +
             " status=200 latency_ms=" + std::to_string(i % 97) + " path=/api/v1/orders\n";
    }
    s += "LAST-LINE-尾标记\n";
    write_file("log.txt", s);
  }

  // report.md：40 节，第 37 节有指标（章节提取的靶子）
  {
    std::string s = "# 年度报告\n\n";
    for (int i = 1; i <= 40; ++i) {
      s += "## Section " + std::to_string(i) + "\n";
      if (i == 37) s += "模型评测结果：召回率 = 0.83，精确率 = 0.91。\n";
      for (int k = 0; k < 12; ++k)
        s += "本节正文段落 " + std::to_string(i) + "-" + std::to_string(k) +
             "：用于拉开篇幅的说明文字，不承载结论。\n";
      s += "\n";
    }
    write_file("report.md", s);
  }

  // config.json：带填充的嵌套结构（第 3 个 service 的端口）
  {
    json doc;
    doc["feature"] = {{"retry", 3}, {"timeout_ms", 800}};
    doc["services"] = json::array();
    for (int i = 0; i < 200; ++i) {
      doc["services"].push_back({{"name", "svc_" + std::to_string(i)},
                                 {"ports", json::array({8000 + i, 9000 + i})},
                                 {"note", "填充条目，用来拉开文件篇幅 " + std::to_string(i)}});
    }
    doc["services"][3] = {{"name", "pay"}, {"ports", json::array({8080, 8443})}};
    write_file("config.json", doc.dump(2));
  }

  // app.yaml：嵌套 + 块列表（server.ports.1）
  {
    std::string s = "server:\n  host: localhost\n  ports:\n    - 80\n    - 443\n";
    for (int i = 0; i < 1500; ++i)
      s += "filler_" + std::to_string(i) + ": \"填充条目 " + std::to_string(i) + "\"\n";
    write_file("app.yaml", s);
  }

  // feed.xml：20 个 entry，第 15 个标题是答案
  {
    std::string s = "<feed>\n";
    for (int i = 1; i <= 20; ++i) {
      s += "  <entry><id>" + std::to_string(i) + "</id><title>";
      s += (i == 15) ? "量子计算取得突破" : ("新闻条目 " + std::to_string(i));
      s += "</title><body>正文 " + std::to_string(i) + "</body></entry>\n";
    }
    s += "</feed>\n";
    write_file("feed.xml", s);
  }

  // novel.txt：60 章，第 12 章出现"李英琼"；结尾"全书完"（采样靶子）
  {
    std::string s;
    for (int i = 1; i <= 60; ++i) {
      s += "第" + std::to_string(i) + "章 江湖夜雨\n";
      if (i == 12) s += "李英琼提剑走入雨里，众人皆惊。\n";
      for (int k = 0; k < 18; ++k)
        s += "山中夜雨，剑气纵横，众人沿石阶向前，不知前路如何（第" + std::to_string(i) +
             "章第" + std::to_string(k) + "段）。\n";
      s += "\n";
    }
    s += "全书完。\n";
    write_file("novel.txt", s);
  }
}

// ---- 各方法：跑一次，返回 {hit, calls, bytes} ----

std::string text_of(json const& r) {
  if (r.contains("text") && r["text"].is_string()) return r["text"].get<std::string>();
  if (r.contains("content") && r["content"].is_string()) return r["content"].get<std::string>();
  return {};
}

Arm run_grep(ToolSpec const& spec, Task const& t) {
  Arm a{true, false, 1, 0};
  ToolContext ctx;
  auto r = spec.handler(json{{"path", t.corpus}, {"pattern", t.needle}}, ctx);
  if (!r) return a;
  for (json const& m : r->at("matches")) {
    a.bytes += m.at("text").get<std::string>().size();
    if (m.at("text").get<std::string>().find(t.answer) != std::string::npos) a.hit = true;
  }
  return a;
}

Arm run_extract(ToolSpec const& spec, Task const& t) {
  Arm a{true, false, 1, 0};
  ToolContext ctx;
  auto r = spec.handler(json{{"path", t.corpus}, {"selector", t.selector}}, ctx);
  if (!r) return a;
  std::string const text = text_of(*r);
  a.bytes = text.size();
  a.hit = r->value("found", false) && text.find(t.answer) != std::string::npos;
  return a;
}

Arm run_sample(ToolSpec const& spec, Task const& t) {
  Arm a{true, false, 1, 0};
  ToolContext ctx;
  auto r = spec.handler(json{{"path", t.corpus}, {"chars_per_window", 400}}, ctx);
  if (!r) return a;
  for (json const& w : r->at("windows")) {
    std::string const text = w.at("text").get<std::string>();
    a.bytes += text.size();
    if (text.find(t.answer) != std::string::npos) a.hit = true;
  }
  return a;
}

// 翻页：8000 字节一窗，直到命中或读完全文（calls 上限 400）
Arm run_paging(ToolSpec const& spec, Task const& t) {
  Arm a{true, false, 0, 0};
  long long offset = 0;
  for (int guard = 0; guard < 400; ++guard) {
    ToolContext ctx;
    auto r = spec.handler(
        json{{"path", t.corpus}, {"offset", offset}, {"max_chars", 8000}}, ctx);
    if (!r) return a;
    ++a.calls;
    std::string content = r->at("content").get<std::string>();
    if (r->at("truncated").get<bool>() && content.size() >= std::string("…").size())
      content.resize(content.size() - std::string("…").size());
    a.bytes += content.size();
    if (content.find(t.answer) != std::string::npos) { a.hit = true; break; }
    if (r->at("next_offset").is_null()) break;
    offset = r->at("next_offset").get<long long>();
  }
  return a;
}

struct Row {
  Task task;
  Arm grep, extract, sample, paging, hybrid;
  bool hybrid_ok = false;
};

}  // namespace

int main() {
  g_root = "demo-out/file_eval_corpus";
  build_corpus();
  std::println("== problems/02 七法统一评测（离线确定性）==");
  std::println("语料目录：{}  ·  指标：hit=答案进入返回文本；calls=工具轮数；bytes=返回字节\n",
               g_root.generic_string());

  FileToolsConfig cfg;
  cfg.root_dir = g_root;
  auto const tools = make_file_tools(cfg);
  ToolSpec const& read_tool = tools[0];
  ToolSpec const& grep_tool = tools[3];
  ToolSpec const& extract_tool = tools[4];
  ToolSpec const& sample_tool = tools[5];

  std::vector<Task> const tasks = {
      {"T1 log_error", "log.txt", "code=0x9F3A", "0x9F3A", "", "", "我知道要找的字面", "grep"},
      {"T2 json_ports", "config.json", "8443", "8443", "json", "/services/3/ports/1",
       "我知道结构（JSON 指针）", "extract"},
      {"T3 yaml_ports", "app.yaml", "443", "443", "yaml", "server.ports.1",
       "我知道结构（YAML 键路径）", "extract"},
      {"T4 xml_title", "feed.xml", "量子计算", "量子计算", "xml", "feed/entry[15]/title",
       "我知道结构（XML 元素路径）", "extract"},
      {"T5 md_section", "report.md", "召回率 = 0.83", "召回率", "markdown", "Section 37",
       "我知道章节标题", "extract"},
      {"T6 novel_chapter", "novel.txt", "李英琼", "李英琼", "markdown", "第12章",
       "我知道章节标题（中文）", "extract"},
      {"T7 ending", "novel.txt", "全书完", "", "", "", "没有先验，只要全局印象", "sample"},
      {"T8 last_line", "log.txt", "LAST-LINE-尾标记", "", "", "", "必须顺序读全（完整性）",
       "paging"},
  };

  std::vector<Row> rows;
  int failures = 0;
  auto check = [&](bool ok, std::string const& what) {
    if (!ok) { std::println("  [✗] {}", what); ++failures; }
    else std::println("  [✓] {}", what);
  };

  for (Task const& t : tasks) {
    std::println("— {}（{}）—", t.id, t.intent);
    Row row{t};

    if (!t.needle.empty()) row.grep = run_grep(grep_tool, t);
    if (!t.format.empty()) row.extract = run_extract(extract_tool, t);
    row.sample = run_sample(sample_tool, t);
    row.paging = run_paging(read_tool, t);

    // 混合路由：按问题形态选 1 法（extract / grep / sample / paging）
    if (t.route == "extract") row.hybrid = row.extract;
    else if (t.route == "grep") row.hybrid = row.grep;
    else if (t.route == "paging") row.hybrid = row.paging;
    else row.hybrid = row.sample;
    row.hybrid_ok = row.hybrid.hit;

    auto cell = [](Arm const& a) {
      if (!a.applicable) return std::string("—");
      return (a.hit ? "✓" : "✗") + std::to_string(a.calls) + "call/" +
             std::to_string(a.bytes) + "B";
    };
    std::println("  grep:{:<14} extract:{:<14} sample:{:<14} paging:{:<14} hybrid:{}",
                 cell(row.grep), cell(row.extract), cell(row.sample), cell(row.paging),
                 cell(row.hybrid));
    rows.push_back(std::move(row));
    std::println("");
  }

  // ---- 断言：每个任务"正确的方法"必须命中；不适用/错方法允许不命中 ----
  std::println("== 断言 ==");
  auto by_id = [&](std::string const& id) -> Row const& {
    for (auto const& r : rows)
      if (r.task.id.rfind(id, 0) == 0) return r;
    return rows.front();
  };
  check(by_id("T1").grep.hit, "T1 字面定位：grep 命中且 1 轮");
  check(by_id("T1").grep.calls == 1, "T1 grep 恰 1 轮（不整份读）");
  check(by_id("T1").sample.hit == false, "T1 采样漏掉中段答案（如实的局限）");
  check(by_id("T2").extract.hit && by_id("T2").extract.calls == 1, "T2 JSON 结构提取 1 轮命中");
  check(by_id("T3").extract.hit, "T3 YAML 键路径命中");
  check(by_id("T4").extract.hit, "T4 XML 元素路径命中");
  check(by_id("T5").extract.hit, "T5 Markdown 章节提取命中");
  check(by_id("T6").extract.hit, "T6 中文章节标题命中");
  check(by_id("T7").sample.hit, "T7 无先验：采样在 tail 命中");
  check(by_id("T8").paging.hit, "T8 顺序读全：翻页最终命中");
  check(by_id("T8").paging.calls > 20, "T8 翻页轮数是完整性的代价（>20 轮）");
  for (auto const& r : rows) check(r.hybrid_ok, "混合路由命中 " + r.task.id);

  // ---- 汇总 ----
  std::println("\n== 方法汇总（仅统计适用任务）==");
  auto aggregate = [&](char const* name, Arm Row::*arm) {
    std::size_t n = 0, hit = 0, calls = 0, bytes = 0;
    for (auto const& r : rows) {
      Arm const& a = r.*arm;
      if (!a.applicable) continue;
      ++n;
      hit += a.hit ? 1 : 0;
      calls += a.calls;
      bytes += a.bytes;
    }
    if (n > 0)
      std::println("  {:<8} 适用 {:>2} · 命中 {:>2} · 平均 {:.1f} 轮 / {:.0f} 字节",
                   name, n, hit, static_cast<double>(calls) / static_cast<double>(n),
                   static_cast<double>(bytes) / static_cast<double>(n));
  };
  aggregate("grep", &Row::grep);
  aggregate("extract", &Row::extract);
  aggregate("sample", &Row::sample);
  aggregate("paging", &Row::paging);
  std::println("  BM25/向量：见 longdoc 验收（problems/57·59·60，蜀山三问读入 13.83%）");

  std::println("\n验收结论：{}", failures == 0 ? "全部断言通过（七法各就各位、混合全覆盖）"
                                                : "有断言失败，见上方 [✗]");
  return failures == 0 ? 0 : 1;
}
