// demo: longdoc —— 应对超长内容的通用方法（arch 28，离线）
//
// 用蜀山 42 块（30 回，~497KB）验证「结构骨架 + 倒排 + 按需深读」范式：
//   ① longdoc_ingest：单遍扫描每个单元 → 骨架 {blurb, keys} → 落 longdoc.index.json
//      （units + 人物倒排）。这里骨架器用"文本内含已知专名"启发式模拟真实模型提取
//      （离线；真机把同一模型替换成真实 LLM 即可）。
//   ② longdoc_retrieve：每问只倒排命中 top-k 块、只读命中块原文切片 → 进 model 上下文。
//   ③ longdoc_ask：模型依据检索切片回答，带 [单元id] 引用。
// 量化：建索引总字节 vs 每次查询实际读入字节 —— 说明"按需加载"相对"全量进上下文"的节省。
#include <cppchain/message/message.hpp>
#include <cppchain/message/serializer.hpp>
#include <cppchain/model/chat_model.hpp>
#include <cppchain/model/task.hpp>
#include <cppchain/workflow/compiler.hpp>
#include <cppchain/workflow/components.hpp>
#include <cppchain/workflow/ir.hpp>
#include <cppchain/workflow/validator.hpp>

#include <filesystem>
#include <fstream>
#include <generator>
#include <print>
#include <string>
#include <vector>

using namespace cppchain;
using namespace cppchain::workflow;
using cppchain::message::Message;

namespace {

// 码点安全的截断（≤60 个字符；UTF-8 多字节按边界切，不产坏字节）。
std::string blurb_of(std::string const& s) {
  std::size_t const e = s.find('\n');
  std::string const line = e == std::string::npos ? s : s.substr(0, e);
  std::string out;
  std::size_t n = 0;
  for (std::size_t i = 0; i < line.size() && n < 60;) {
    unsigned char const c = static_cast<unsigned char>(line[i]);
    std::size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    if (i + len > line.size()) len = line.size() - i;
    out.append(line, i, len);
    i += len;
    ++n;
  }
  return out;
}

// 骨架器 / 回答器：脚本化（离线）。骨架器按"文本内含专名"产 keys（模拟真实提取）；
// 回答器按问题回一句带引用的答复。
class ScriptedLongdoc final : public model::ChatModel {
 public:
  bool is_skel = false;
  std::vector<std::string> seen_systems;
  std::expected<Message, model::ModelError> invoke(std::span<Message const> msgs,
                                                   model::CallOptions const&) override {
    for (Message const& m : msgs)
      if (m.type == message::Role::System) seen_systems.push_back(m.text());
    std::string human;
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it)
      if (it->type == message::Role::Human) {
        human = it->text();
        break;
      }
    if (is_skel) {
      // 模拟结构提取：出现哪些已知专名（含别名归并）→ keys；blurb = 前 60 字符
      std::vector<std::string> keys;
      for (std::string const& e : {"李英琼", "李宁", "周淳", "毛太", "妙通"})
        if (human.find(e) != std::string::npos) keys.push_back(e);
      if (human.find("英琼") != std::string::npos &&
          std::find(keys.begin(), keys.end(), "李英琼") == keys.end())
        keys.push_back("李英琼");  // 别名归并（原文多称"英琼"）
      Json sk = Json{{"blurb", blurb_of(human)}};
      sk["keys"] = keys;
      return message::ai_message(sk.dump());
    }
    // 回答器：按问题回一句（演示引用协议；真机由真实模型作答）。
    // 用户问题经 retrieve 写进 system_context（【用户问题】…），这里从 system 里判断。
    std::string joined;
    for (std::string const& s : seen_systems) joined += s;
    if (joined.find("父亲") != std::string::npos)
      return message::ai_message("李英琼的父亲是李宁（第 1 块入川同行）[block_001_1.txt]");
    if (joined.find("结怨") != std::string::npos)
      return message::ai_message("周淳与毛太结怨（第 1 块附近交代）[block_001_1.txt]");
    return message::ai_message("请查看命中的检索片段作答 [block_001_1.txt]");
  }
  std::generator<message::MessageChunk> stream(std::span<Message const>,
                                               model::CallOptions const&) override {
    co_return;
  }
  core::CancelToken stream_async(std::span<Message const>, model::CallOptions const&,
                                 model::StreamHandlers) override {
    return {};
  }
  model::Task<std::expected<Message, model::ModelError>> invoke_async(std::span<Message const>,
                                                                      model::CallOptions) override {
    return {};
  }
};

Json ingest_wf() {
  return Json::parse(R"json({
    "id": "wf_longdoc_ingest",
    "nodes": [
      {"id": "ing", "kind": "longdoc_ingest", "longdoc_root": "@config.longdoc_root",
       "longdoc_index": "@config.longdoc_index", "model": "m", "next": "_turn_end"}
    ]
  })json");
}
Json ask_wf() {
  return Json::parse(R"json({
    "id": "wf_longdoc_ask",
    "nodes": [
      {"id": "find", "kind": "longdoc_retrieve", "longdoc_index": "@config.longdoc_index",
       "longdoc_query": "@config.query", "longdoc_topk": "@config.longdoc_topk",
       "longdoc_slice_chars": "@config.longdoc_slice_chars", "next": "answer"},
      {"id": "answer", "kind": "model", "model": "m",
       "system_prompt": "依据【检索片段】回答，引用 [单元id]，不臆造。", "next": "_turn_end"}
    ]
  })json");
}

}  // namespace

int main() {
  std::filesystem::path const blocks = std::filesystem::path("tmp") / "wfr" / "corpus" /
                                       "shushan" / "blocks";
  if (!std::filesystem::is_directory(blocks)) {
    std::println("蜀山语料缺失: {}（可先抓 42 块）", blocks.string());
    return 1;
  }
  std::filesystem::path const idx = std::filesystem::path("tmp") / "longdoc_demo.index.json";

  ScriptedLongdoc skel;
  skel.is_skel = true;
  ComponentRegistry comps;
  comps.models["m"] = &skel;

  tool::ToolRegistry reg;
  auto compile = [&](Json ir) -> CompiledWorkflow {
    auto parsed = WorkflowIR::parse(ir);
    if (!parsed) {
      std::println("IR 解析失败: {}", parsed.error());
      std::exit(1);
    }
    auto v = validate(*parsed, ValidatorOptions{});
    if (!v.ok()) {
      std::println("校验失败: {}", v.describe());
      std::exit(1);
    }
    CompileOptions copts;
    copts.components = &comps;
    auto wf = compile_workflow(*parsed, reg, nullptr, copts);
    if (!wf) {
      std::println("编译失败: {}", wf.error().message);
      std::exit(1);
    }
    return std::move(*wf);
  };

  // ① 建索引（结构骨架 + 人物倒排）
  auto wf_ing = compile(ingest_wf());
  std::uintmax_t corpus_bytes = 0;
  for (std::filesystem::directory_iterator it(blocks); it != std::filesystem::directory_iterator();
       ++it)
    if (it->is_regular_file()) corpus_bytes += it->file_size();
  auto ing = fire_once(wf_ing, Json{{"longdoc_root", blocks.string()},
                                    {"longdoc_index", idx.string()}});
  if (!ing) {
    std::println("建索引失败: {}", ing.error().message);
    return 1;
  }
  std::println("=== longdoc demo（arch 28 · 超长内容通用方法 · 离线）===");
  std::println("① 建索引: {} 单元（{} 字节 ≈ 全量 token 成本），索引 {}",
               (*ing)["results"]["ing"]["units"].get<long>(), corpus_bytes, idx.string());

  // ②③ 每问：倒排命中 top-k → 只读命中块切片 → 回答（回答器独立实例，is_skel=false）
  ScriptedLongdoc answerer;  // 回答器：按问题回带引用的答复
  comps.models["m"] = &answerer;
  auto wf_ask = compile(ask_wf());
  const char* questions[] = {"李英琼的父亲是谁？", "周淳与谁结怨？", "妙通是谁？"};
  std::uintmax_t total_read = 0;
  std::size_t total_calls = 0;
  for (char const* q : questions) {
    answerer.seen_systems.clear();
    auto r = fire_once(wf_ask, Json{{"longdoc_index", idx.string()}, {"query", q},
                                    {"longdoc_topk", "2"}, {"longdoc_slice_chars", "3000"}});
    if (!r) {
      std::println("  ✗ 查询失败: {}", r.error().message);
      continue;
    }
    auto const& hits = (*r)["results"]["find"]["hits"];
    std::uintmax_t read = 0;
    std::string ids;
    for (Json const& h : hits) {
      read += h["bytes"].get<std::uintmax_t>();
      ids += (ids.empty() ? "" : ", ") + h["id"].get<std::string>();
    }
    total_read += read;
    total_calls += hits.size();
    std::string answer;
    auto const& msgs = (*r).value("messages", Json::array());
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it)
      if ((*it).value("type", "") == "ai" && (*it).contains("content") &&
          (*it)["content"].is_array() && !(*it)["content"].empty()) {
        answer = (*it)["content"][0].value("text", "");
        break;
      }
    std::println("② 问「{}」 → 命中 {} 块 [{}]（读入 {} 字节）", q, hits.size(), ids, read);
    std::println("③ 答: {}", answer);
  }
  std::println("量化: 全量 {} 字节 vs 三问共读入 {} 字节（按需加载 ≈ 全量的 {:.2f}%）",
               corpus_bytes, total_read,
               100.0 * static_cast<double>(total_read) / static_cast<double>(corpus_bytes));
  std::println("（真机只需把 ScriptedLongdoc 换成真实 LLM 骨架器/回答器，机制不变）");
  return 0;
}