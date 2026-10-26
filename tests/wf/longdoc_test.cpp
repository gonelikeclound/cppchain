// tests/wf —— longdoc：应对超长内容的通用方法（arch 28，机制层）
//   覆盖：ingest 规则/模型档建索引（units + keys 倒排）、retrieve 键命中 top-k + 切片预算截断、
//   system_context 注入、无命中 note、ask 闭环（模型带引用）、机制与语料无关（假语料）、
//   蜀山 42 块规则 ingest（结构导航成立）。
#include <gtest/gtest.h>

#include <deque>
#include <filesystem>
#include <fstream>
#include <generator>
#include <random>
#include <string>
#include <vector>

#include "cppchain/message/message.hpp"
#include "cppchain/model/chat_model.hpp"
#include "cppchain/model/task.hpp"
#include "cppchain/workflow/compiler.hpp"
#include "cppchain/workflow/components.hpp"
#include "cppchain/workflow/ir.hpp"
#include "cppchain/workflow/validator.hpp"

using namespace cppchain;
using namespace cppchain::workflow;
using cppchain::message::Message;

namespace {

// 脚本化模型：按队列吐回复；记录收到的 system 文本（验证检索片段进了模型上下文）。
class ScriptedLongdoc final : public model::ChatModel {
 public:
  std::deque<Message> replies;
  std::vector<std::string> seen_systems;
  std::expected<Message, model::ModelError> invoke(std::span<Message const> msgs,
                                                   model::CallOptions const&) override {
    for (Message const& m : msgs)
      if (m.type == message::Role::System) seen_systems.push_back(m.text());
    if (replies.empty())
      return std::unexpected(
          model::ModelError{model::ModelError::Kind::Parse, 0, "replies exhausted", {}});
    Message m = replies.front();
    replies.pop_front();
    return m;
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

struct LongdocFixture {
  std::filesystem::path ws;
  std::filesystem::path corpus;
  LongdocFixture()
      : ws(std::filesystem::temp_directory_path() /
           ("wf_longdoc_" + std::to_string(static_cast<unsigned>(std::random_device{}())))),
        corpus(ws / "corpus") {
    std::filesystem::create_directories(corpus);
    // 与语料无关的假语料：两中文块（人物主题）+ 一块纯 ASCII
    write("a.txt", "第一章 李宁带着女儿李英琼入川。周淳与李宁是多年故交，共商避祸。\n");
    write("b.txt", "周淳与毛太结怨多年，毛太扬言寻仇。\n");
    write("c.txt", "alpha beta gamma delta epsilon\n");
  }
  ~LongdocFixture() {
    std::error_code ec;
    std::filesystem::remove_all(ws, ec);
  }
  void write(std::string const& name, std::string const& text) const {
    std::ofstream out(corpus / name);
    out << text;
  }
  std::filesystem::path index_path() const { return ws / "longdoc.index.json"; }
  Json read_index() const {
    std::ifstream in(index_path());
    Json j;
    in >> j;
    return j;
  }
};

struct Run {
  std::optional<Json> state;
  std::string error;
};
Run fire(Json ir, Json config, CompileOptions const& copts) {
  auto parsed = WorkflowIR::parse(ir);
  if (!parsed) return {std::nullopt, "parse: " + parsed.error()};
  auto v = validate(*parsed, ValidatorOptions{});
  if (!v.ok()) return {std::nullopt, "validate: " + v.describe()};
  tool::ToolRegistry reg;
  auto wf = compile_workflow(*parsed, reg, nullptr, copts);
  if (!wf) return {std::nullopt, "compile: " + wf.error().message};
  auto r = fire_once(*wf, config);
  if (!r) return {std::nullopt, "fire: " + r.error().message};
  return {std::optional<Json>{*r}, ""};
}

Json ingest_ir() {
  return Json::parse(R"json({
    "id": "wf_longdoc_ingest",
    "nodes": [
      {"id": "ing", "kind": "longdoc_ingest", "longdoc_root": "@config.longdoc_root",
       "longdoc_index": "@config.longdoc_index", "model": "m", "next": "_turn_end"}
    ]
  })json");
}
Json retrieve_ir() {
  return Json::parse(R"json({
    "id": "wf_longdoc_retrieve",
    "nodes": [
      {"id": "find", "kind": "longdoc_retrieve", "longdoc_index": "@config.longdoc_index",
       "longdoc_query": "@config.query", "longdoc_topk": "@config.longdoc_topk",
       "longdoc_slice_chars": "@config.longdoc_slice_chars", "next": "_turn_end"}
    ]
  })json");
}

}  // namespace

// 规则档 ingest：无模型 → blurb=首行、ASCII 语料 keys=词表、索引落盘、倒排正确
TEST(WfLongdoc, ingest_rule_builds_index) {
  LongdocFixture fx;
  // 无 components → 规则档
  auto st = fire(ingest_ir(),
                 Json{{"longdoc_root", fx.corpus.string()},
                      {"longdoc_index", fx.index_path().string()}},
                 CompileOptions{});
  ASSERT_TRUE(st.state) << st.error;
  Json const& r = (*st.state)["results"]["ing"];
  EXPECT_EQ(r["units"], 3L);
  Json const idx = fx.read_index();
  ASSERT_EQ(idx["units"].size(), 3u);
  EXPECT_EQ(idx["units"][0]["id"], "a.txt");
  EXPECT_NE(idx["units"][0]["blurb"].get<std::string>().find("李宁"), std::string::npos);
  // ASCII 块规则 keys；中文块 keys 空（无空格分词，倒排留模型档）
  EXPECT_EQ(idx["units"][2]["keys"].size(), 5u);  // alpha beta gamma delta epsilon
  EXPECT_EQ(idx["units"][0]["keys"].size(), 0u);
  EXPECT_EQ(idx["keys"]["alpha"], Json::array({2}));
}

// 模型档 ingest：每块产 blurb+keys → 倒排按 key → unit 下标
TEST(WfLongdoc, ingest_model_extracts_skeleton) {
  LongdocFixture fx;
  ScriptedLongdoc mdl;
  mdl.replies.push_back(message::ai_message(
      R"({"blurb":"李宁携女入川，遇故交周淳","keys":["李宁","李英琼","周淳"]})"));
  mdl.replies.push_back(message::ai_message(
      R"({"blurb":"周淳与毛太结怨","keys":["周淳","毛太"]})"));
  mdl.replies.push_back(message::ai_message(
      R"({"blurb":"纯词块","keys":["alpha","beta"]})"));
  ComponentRegistry comps;
  comps.models["m"] = &mdl;
  CompileOptions copts;
  copts.components = &comps;
  auto st = fire(ingest_ir(),
                 Json{{"longdoc_root", fx.corpus.string()},
                      {"longdoc_index", fx.index_path().string()}},
                 copts);
  ASSERT_TRUE(st.state) << st.error;
  Json const idx = fx.read_index();
  ASSERT_EQ(idx["units"].size(), 3u);
  EXPECT_EQ(idx["units"][0]["keys"],
            Json::array({"周淳", "李宁", "李英琼"}));  // 排序（码点序：周<李）去重
  // 倒排：周淳 → [0,1]
  EXPECT_EQ(idx["keys"]["周淳"], Json::array({0, 1}));
  EXPECT_EQ(idx["keys"]["毛太"], Json::array({1}));
}

// retrieve：query 命中倒排 → top-k、切片加载（预算截断）、system_context 注入
TEST(WfLongdoc, retrieve_topk_and_slice) {
  LongdocFixture fx;
  ScriptedLongdoc mdl;
  mdl.replies.push_back(message::ai_message(
      R"({"blurb":"李宁携女入川，遇故交周淳","keys":["李宁","李英琼","周淳"]})"));
  mdl.replies.push_back(message::ai_message(
      R"({"blurb":"周淳与毛太结怨","keys":["周淳","毛太"]})"));
  mdl.replies.push_back(message::ai_message(
      R"({"blurb":"纯词块","keys":["alpha","beta"]})"));
  ComponentRegistry comps;
  comps.models["m"] = &mdl;
  CompileOptions copts;
  copts.components = &comps;
  auto st = fire(ingest_ir(),
                 Json{{"longdoc_root", fx.corpus.string()},
                      {"longdoc_index", fx.index_path().string()}},
                 copts);
  ASSERT_TRUE(st.state) << st.error;

  auto rt = fire(retrieve_ir(),
                 Json{{"longdoc_index", fx.index_path().string()},
                      {"query", "李英琼"}, {"longdoc_topk", "3"},
                      {"longdoc_slice_chars", "200"}},
                 CompileOptions{});
  ASSERT_TRUE(rt.state) << rt.error;
  Json const& hits =  (*rt.state)["results"]["find"]["hits"];
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0]["id"], "a.txt");
  EXPECT_NE(hits[0]["text"].get<std::string>().find("李英琼"), std::string::npos);
  // system_context 注入（model 自动带上检索片段）
  ASSERT_TRUE( (*rt.state)[kSystemContextKey].is_array());
  std::string all;
  for (Json const& m :  (*rt.state)[kSystemContextKey])
    if (m.contains("content") && m["content"].is_array())
      for (Json const& b : m["content"])
        if (b.is_object() && b.contains("text")) all += b["text"].get<std::string>() + "\n";
  EXPECT_NE(all.find("【a.txt】"), std::string::npos) << all;
}

// 多单元命中 + topk 收敛：周淳 → a、b 都命中，topk=1 取先
TEST(WfLongdoc, retrieve_respects_topk) {
  LongdocFixture fx;
  ScriptedLongdoc mdl;
  mdl.replies.push_back(message::ai_message(
      R"({"blurb":"李宁携女入川","keys":["李宁","李英琼","周淳"]})"));
  mdl.replies.push_back(message::ai_message(
      R"({"blurb":"周淳与毛太结怨","keys":["周淳","毛太"]})"));
  mdl.replies.push_back(message::ai_message(
      R"({"blurb":"纯词块","keys":["alpha","beta"]})"));
  ComponentRegistry comps;
  comps.models["m"] = &mdl;
  CompileOptions copts;
  copts.components = &comps;
  auto st = fire(ingest_ir(),
                 Json{{"longdoc_root", fx.corpus.string()},
                      {"longdoc_index", fx.index_path().string()}},
                 copts);
  ASSERT_TRUE(st.state) << st.error;
  auto rt = fire(retrieve_ir(),
                 Json{{"longdoc_index", fx.index_path().string()},
                      {"query", "周淳"}, {"longdoc_topk", "1"},
                      {"longdoc_slice_chars", "500"}},
                 CompileOptions{});
  ASSERT_TRUE(rt.state) << rt.error;
  Json const& hits =  (*rt.state)["results"]["find"]["hits"];
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0]["id"], "a.txt");  // 同分按单元序，topk=1 取先
}

// 切片预算截断：text ≤ 预算（UTF-8 安全），bytes 仍为全文
TEST(WfLongdoc, retrieve_slice_budget_truncates) {
  LongdocFixture fx;
  ScriptedLongdoc mdl;
  mdl.replies.push_back(message::ai_message(
      R"({"blurb":"李宁携女入川，遇故交周淳","keys":["李宁","李英琼","周淳"]})"));
  mdl.replies.push_back(message::ai_message(
      R"({"blurb":"周淳与毛太结怨","keys":["周淳","毛太"]})"));
  mdl.replies.push_back(message::ai_message(
      R"({"blurb":"纯词块","keys":["alpha","beta"]})"));
  ComponentRegistry comps;
  comps.models["m"] = &mdl;
  CompileOptions copts;
  copts.components = &comps;
  auto st = fire(ingest_ir(),
                 Json{{"longdoc_root", fx.corpus.string()},
                      {"longdoc_index", fx.index_path().string()}},
                 copts);
  ASSERT_TRUE(st.state) << st.error;
  auto rt = fire(retrieve_ir(),
                 Json{{"longdoc_index", fx.index_path().string()},
                      {"query", "李英琼"}, {"longdoc_topk", "1"},
                      {"longdoc_slice_chars", "6"}},
                 CompileOptions{});
  ASSERT_TRUE(rt.state) << rt.error;
  Json const& h =  (*rt.state)["results"]["find"]["hits"][0];
  std::string const sliced = h["text"].get<std::string>();
  EXPECT_LE(sliced.size(), 6u + 3u);          // utf8 预算 6 字节 + "…"（3 字节）
  EXPECT_TRUE(sliced.ends_with("…"));          // 截断标注
  EXPECT_GT(h["bytes"].get<long long>(), 40ll);  // bytes 是全文长度
}

// 无命中 → hits 空 + note
TEST(WfLongdoc, retrieve_no_hit_note) {
  LongdocFixture fx;
  // 规则档建索引（c.txt 有 ascii keys）
  auto st = fire(ingest_ir(),
                 Json{{"longdoc_root", fx.corpus.string()},
                      {"longdoc_index", fx.index_path().string()}},
                 CompileOptions{});
  ASSERT_TRUE(st.state) << st.error;
  auto rt = fire(retrieve_ir(),
                 Json{{"longdoc_index", fx.index_path().string()},
                      {"query", "不存在的词"}, {"longdoc_topk", "3"},
                      {"longdoc_slice_chars", "100"}},
                 CompileOptions{});
  ASSERT_TRUE(rt.state) << rt.error;
  EXPECT_EQ( (*rt.state)["results"]["find"]["hits"].size(), 0u);
  EXPECT_NE( (*rt.state)["results"]["find"]["note"].get<std::string>().find("无命中"), std::string::npos);
}

// ask 闭环：retrieve 切片进 model 上下文 → 模型回答带 [单元id] 引用
TEST(WfLongdoc, ask_closes_loop_with_citation) {
  LongdocFixture fx;
  ScriptedLongdoc mdl;
  mdl.replies.push_back(message::ai_message(
      R"({"blurb":"李宁携女入川，遇故交周淳","keys":["李宁","李英琼","周淳"]})"));
  mdl.replies.push_back(message::ai_message(
      R"({"blurb":"周淳与毛太结怨","keys":["周淳","毛太"]})"));
  mdl.replies.push_back(message::ai_message(
      R"({"blurb":"纯词块","keys":["alpha","beta"]})"));
  mdl.replies.push_back(message::ai_message("李英琼的父亲是李宁。 [a.txt]"));  // ask 的答复
  ComponentRegistry comps;
  comps.models["m"] = &mdl;
  CompileOptions copts;
  copts.components = &comps;
  auto st = fire(ingest_ir(),
                 Json{{"longdoc_root", fx.corpus.string()},
                      {"longdoc_index", fx.index_path().string()}},
                 copts);
  ASSERT_TRUE(st.state) << st.error;

  auto ask = Json::parse(R"json({
    "id": "wf_longdoc_ask",
    "nodes": [
      {"id": "find", "kind": "longdoc_retrieve", "longdoc_index": "@config.longdoc_index",
       "longdoc_query": "@config.query", "longdoc_topk": "@config.longdoc_topk",
       "longdoc_slice_chars": "@config.longdoc_slice_chars", "next": "answer"},
      {"id": "answer", "kind": "model", "model": "m",
       "system_prompt": "依据【检索片段】回答，引用 [单元id]，不臆造。", "next": "_turn_end"}
    ]
  })json");
  auto r = fire(ask,
                Json{{"longdoc_index", fx.index_path().string()},
                     {"query", "李英琼的父亲是谁"}, {"longdoc_topk", "1"},
                     {"longdoc_slice_chars", "200"}},
                copts);
  ASSERT_TRUE(r.state) << r.error;
  // model 收到的 system 含检索片段（【a.txt】）
  bool saw_slice = false;
  for (std::string const& s : mdl.seen_systems)
    if (s.find("【a.txt】") != std::string::npos) saw_slice = true;
  EXPECT_TRUE(saw_slice) << "检索片段应进 model 上下文（system_context）";
  // 答复带引用
  std::string last_ai;
  auto const msgs =  (*r.state).value("messages", Json::array());
  for (auto it = msgs.rbegin(); it != msgs.rend(); ++it)
    if ((*it).value("type", "") == "ai") {
      last_ai = (*it)["content"].dump();
      break;
    }
  EXPECT_NE(last_ai.find("[a.txt]"), std::string::npos) << last_ai;
}

// 通用性：机制零蜀山依赖（假语料走完整流程已覆盖）；蜀山 42 块规则 ingest（结构导航成立）
TEST(WfLongdoc, shushan_rule_ingest_42_units) {
  std::filesystem::path const blocks = std::filesystem::path("tmp") / "wfr" / "corpus" /
                                       "shushan" / "blocks";
  if (!std::filesystem::is_directory(blocks)) GTEST_SKIP() << "蜀山语料未在 tmp/（可重抓）";
  std::filesystem::path const idx =
      std::filesystem::temp_directory_path() /
      ("wf_longdoc_shushan_" + std::to_string(static_cast<unsigned>(std::random_device{}())) +
       ".index.json");
  auto st = fire(ingest_ir(),
                 Json{{"longdoc_root", blocks.string()}, {"longdoc_index", idx.string()}},
                 CompileOptions{});
  ASSERT_TRUE(st.state) << st.error;
  EXPECT_EQ((*st.state)["results"]["ing"]["units"], 42L);
  Json j = [&] {
    std::ifstream in(idx);
    Json x; in >> x; return x;
  }();
  EXPECT_EQ(j["units"].size(), 42u);
  EXPECT_EQ(j["units"][0]["id"], "block_001_1.txt");
  EXPECT_NE(j["units"][0]["blurb"].get<std::string>().find("峨眉"), std::string::npos);
  std::error_code ec;
  std::filesystem::remove(idx, ec);
}
// ================= 分块策略进阶（arch 28 下一刀）：固定窗口 / 章节 =================

namespace {
struct ChunkCorpus {
  std::filesystem::path dir;
  explicit ChunkCorpus(std::string tag) {
    dir = std::filesystem::temp_directory_path() / ("wf_longdoc_chunk_" + tag + "_" +
                                                    std::to_string(static_cast<unsigned>(std::random_device{}())));
    std::filesystem::create_directories(dir);
  }
  ~ChunkCorpus() {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
  void write(std::string const& name, std::string const& text) const {
    std::ofstream out(dir / name);
    out << text;
  }
  std::filesystem::path index_path() const { return dir / "longdoc.index.json"; }
  Json read_index() const {
    std::ifstream in(index_path());
    Json j;
    in >> j;
    return j;
  }
};

// 固定窗口：~300 词 → 多块；retrieve 只按 offset/length 读命中的那一块（不是整文件）。
TEST(WfLongdoc, fixed_window_chunks_and_range_retrieve) {
  ChunkCorpus c("fixed");
  std::string big;
  for (int i = 0; i < 10; ++i) {
    big += "wordA" + std::to_string(i) + " wordB" + std::to_string(i) + " common word\n";
  }  // 10 行 ≈ 300 字符，逐行词不同
  c.write("big.txt", big);
  Json const cfg = Json{{"longdoc_root", c.dir.string()},
                        {"longdoc_index", c.index_path().string()},
                        {"longdoc_unit_of", "fixed"},
                        {"longdoc_chunk_chars", 120},
                        {"longdoc_chunk_overlap", 20},
                        {"query", "wordA7"},
                        {"longdoc_topk", 2},
                        {"longdoc_slice_chars", 2000}};
  auto r = fire(ingest_ir(), cfg, CompileOptions{});
  ASSERT_TRUE(r.state) << r.error;
  Json const idx = c.read_index();  // 自己读
  EXPECT_EQ(idx["meta"]["unit_of"], "fixed");
  auto const& units = idx["units"];
  ASSERT_GT(units.size(), 1u);  // 120 字符窗 / step100 → 至少 3 块
  // 偏移单调、首 0、末块终点 == 文件字节
  long long prev_off = -1;
  for (auto const& u : units) {
    long long const off = u["offset"].get<long long>();
    EXPECT_GT(off, prev_off);
    prev_off = off;
    EXPECT_TRUE(u.contains("bytes") && u["bytes"].get<long long>() > 0);
  }
  EXPECT_EQ(units[0]["offset"].get<long long>(), 0LL);

  // retrieve：query wordA7 → 命中含 wordA7 的那块，且只读该块范围（文本不含别的窗的词）
  auto q = fire(retrieve_ir(), cfg, CompileOptions{});
  ASSERT_TRUE(q.state) << q.error;
  Json const& hits = (*q.state)["results"]["find"]["hits"];
  ASSERT_GT(hits.size(), 0u);
  std::string const text = hits[0]["text"].get<std::string>();
  EXPECT_NE(text.find("wordA7"), std::string::npos);
  EXPECT_EQ(text.find("wordA9"), std::string::npos);  // 不在命中块的窗，没被读进来
  // 命中块 offset 与索引里含 wordA7 的那块一致（只读该块范围）
  bool offset_aligned = false;
  for (auto const& u : units)
    if (u["offset"].get<long long>() == hits[0]["offset"].get<long long>()) offset_aligned = true;
  EXPECT_TRUE(offset_aligned) << "hits.offset 应对应索引中某一块";
}

// 章节：按 第X回/章 切块，标题=标题行，retrieve 只命中目标块。
TEST(WfLongdoc, chapter_headings_split_and_title) {
  ChunkCorpus c("chapter");
  c.write("novel.txt",
          "第1回 Alpha Story\nalpha beta content here.\n\n第2回 Beta Story\n"
          "gamma delta content here.\n\n第3回 Gamma Story\nepsilon zeta content here.\n");
  Json const cfg = Json{{"longdoc_root", c.dir.string()},
                        {"longdoc_index", c.index_path().string()},
                        {"longdoc_unit_of", "chapter"},
                        {"query", "gamma"},
                        {"longdoc_topk", 1},
                        {"longdoc_slice_chars", 2000}};
  auto r = fire(ingest_ir(), cfg, CompileOptions{});
  ASSERT_TRUE(r.state) << r.error;
  Json const idx = c.read_index();
  EXPECT_EQ(idx["meta"]["unit_of"], "chapter");
  auto const& units = idx["units"];
  ASSERT_EQ(units.size(), 3u);
  EXPECT_EQ(units[0]["title"], "第1回 Alpha Story");
  EXPECT_EQ(units[1]["title"], "第2回 Beta Story");
  EXPECT_EQ(units[2]["title"], "第3回 Gamma Story");
  EXPECT_EQ(units[1]["id"], "novel.txt!2");

  auto q = fire(retrieve_ir(), cfg, CompileOptions{});
  ASSERT_TRUE(q.state) << q.error;
  Json const& hits = (*q.state)["results"]["find"]["hits"];
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0]["id"], "novel.txt!2");        // gamma 在第二回
  EXPECT_NE(hits[0]["text"].get<std::string>().find("gamma"), std::string::npos);
  EXPECT_EQ(hits[0]["text"].get<std::string>().find("alpha"), std::string::npos);  // 只读第二回
}

// 章节无标题 → 退化整文件一个块（fail-open，不丢数据）。
TEST(WfLongdoc, chapter_no_heading_falls_back_whole) {
  ChunkCorpus c("nohdr");
  c.write("plain.txt", "hello world no headings here\nsecond line\n");
  Json const cfg = Json{{"longdoc_root", c.dir.string()},
                        {"longdoc_index", c.index_path().string()},
                        {"longdoc_unit_of", "chapter"}};
  auto r = fire(ingest_ir(), cfg, CompileOptions{});
  ASSERT_TRUE(r.state) << r.error;
  Json const idx = c.read_index();
  ASSERT_EQ(idx["units"].size(), 1u);
  EXPECT_EQ(idx["units"][0]["id"], "plain.txt");
  EXPECT_EQ(idx["units"][0]["offset"], 0);
}

// 固定窗口边界 UTF-8 安全：分块起点绝不能落在多字节字符中间。
TEST(WfLongdoc, fixed_window_boundaries_utf8_safe) {
  ChunkCorpus c("utf8");
  std::string zh;
  for (int i = 0; i < 60; ++i) zh += "蜀山剑侠传李英琼";  // 60×3字，全中文
  c.write("zh.txt", zh);
  Json const cfg = Json{{"longdoc_root", c.dir.string()},
                        {"longdoc_index", c.index_path().string()},
                        {"longdoc_unit_of", "fixed"},
                        {"longdoc_chunk_chars", 7},   // 7 字窗，必然横跨 3 字节中文字
                        {"longdoc_chunk_overlap", 1}};
  auto r = fire(ingest_ir(), cfg, CompileOptions{});
  ASSERT_TRUE(r.state) << r.error;
  // 读原文件字节
  std::ifstream in(c.dir / "zh.txt", std::ios::binary);
  std::string const bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  Json const idx = c.read_index();
  for (auto const& u : idx["units"]) {
    std::size_t const off = static_cast<std::size_t>(u["offset"].get<long long>());
    ASSERT_LT(off, bytes.size());
    unsigned char const lead = static_cast<unsigned char>(bytes[off]);
    EXPECT_FALSE(lead >= 0x80 && lead <= 0xBF)
        << "分块起点落在多字节字符中间（续字节），offset=" << off;
  }
}
}  // namespace

// ================= 向量臂（arch 28 下一刀）：blurb 语义检索 =================

namespace {
// 确定性假 embedding：256 维"字符袋"向量（字节索引计数）。共享字符 → 余弦高，够测语义命中。
class BagEmbeddings final : public model::Embeddings {
 public:
  std::expected<std::vector<model::Embedding>, model::ModelError> embed(
      std::span<std::string const> texts) override {
    std::vector<model::Embedding> out;
    for (std::string const& t : texts) out.push_back(vector_for(t));
    return out;
  }
  std::size_t dimension() const override { return 256; }
  std::size_t max_batch_size() const override { return 1000; }
  static model::Embedding vector_for(std::string_view s) {
    model::Embedding v(256, 0.0f);
    for (unsigned char c : s) v[c] += 1.0f;
    return v;
  }
};
}  // namespace

// ingest 带向量：units 每项出 emb、meta.embed 记组件名。
TEST(WfLongdoc, ingest_vector_embeds_unit_blurbs) {
  LongdocFixture fx;
  BagEmbeddings bag;
  ComponentRegistry comps;
  comps.embeddings["bag"] = &bag;
  CompileOptions copts;
  copts.components = &comps;
  auto st = fire(ingest_ir(),
                 Json{{"longdoc_root", fx.corpus.string()},
                      {"longdoc_index", fx.index_path().string()},
                      {"longdoc_embed", "bag"}},
                 copts);
  ASSERT_TRUE(st.state) << st.error;
  Json const idx = fx.read_index();
  EXPECT_EQ(idx["meta"]["embed"], "bag");
  ASSERT_EQ(idx["units"].size(), 3u);
  for (Json const& u : idx["units"]) {
    ASSERT_TRUE(u.contains("emb"));
    EXPECT_EQ(u["emb"].size(), 256u);
  }
}

// 泛问：关键词在 keys 里没有（中文规则档 keys 空），但"女儿"与 a.txt blurb 字面重叠
// → 向量路径命中 a.txt（纯向量，无关键词命中也能答）。
TEST(WfLongdoc, retrieve_vector_resolves_abstract_query) {
  LongdocFixture fx;
  BagEmbeddings bag;
  ComponentRegistry comps;
  comps.embeddings["bag"] = &bag;
  CompileOptions copts;
  copts.components = &comps;
  auto st = fire(ingest_ir(),
                 Json{{"longdoc_root", fx.corpus.string()},
                      {"longdoc_index", fx.index_path().string()},
                      {"longdoc_embed", "bag"}},
                 copts);
  ASSERT_TRUE(st.state) << st.error;
  auto q = fire(retrieve_ir(),
                Json{{"longdoc_index", fx.index_path().string()},
                     {"query", "女儿"}, {"longdoc_topk", 3},
                     {"longdoc_slice_chars", 2000}, {"longdoc_vector_weight", 0.5}},
                copts);
  ASSERT_TRUE(q.state) << q.error;
  Json const& res = (*q.state)["results"]["find"];
  EXPECT_EQ(res["vector"], true);
  auto const& hits = res["hits"];
  ASSERT_GT(hits.size(), 0u);
  EXPECT_EQ(hits[0]["id"], "a.txt");  // 女儿在 a.txt 的 blurb（首行）
}

// 无 embedding 组件 → 退纯关键词：泛问"女儿"零命中 + vector=false（向后兼容行为）。
TEST(WfLongdoc, retrieve_vector_disabled_without_embedder) {
  LongdocFixture fx;
  auto st = fire(ingest_ir(),
                 Json{{"longdoc_root", fx.corpus.string()},
                      {"longdoc_index", fx.index_path().string()}},
                 CompileOptions{});
  ASSERT_TRUE(st.state) << st.error;
  EXPECT_EQ(fx.read_index()["meta"]["embed"], "");
  auto q = fire(retrieve_ir(),
                Json{{"longdoc_index", fx.index_path().string()},
                     {"query", "女儿"}, {"longdoc_topk", 3}, {"longdoc_slice_chars", 2000}},
                CompileOptions{});
  ASSERT_TRUE(q.state) << q.error;
  Json const& res = (*q.state)["results"]["find"];
  EXPECT_EQ(res["vector"], false);
  EXPECT_EQ(res["hits"].size(), 0u);
}

// ================= 人物关系 / 故事线索（longdoc_relation）=================

namespace {
Json relation_ir() {
  return Json::parse(R"json({"id":"w","nodes":[{"id":"rel","kind":"longdoc_relation",
    "longdoc_index":"@config.longdoc_index","longdoc_query":"@config.longdoc_rel_query",
    "next":"_turn_end"}]})json");
}
// 三份带 relations/thread 的模型回复（复用 ScriptedLongdoc）。
void push_rel_replies(ScriptedLongdoc& mdl) {
  mdl.replies.push_back(message::ai_message(
      R"({"blurb":"父女入川","keys":["李宁","李英琼"],"relations":[{"a":"李宁","b":"李英琼","rel":"父女"},{"a":"李宁","b":"周淳","rel":"故交"}],"thread":"父女入川遇故交"})"));
  mdl.replies.push_back(message::ai_message(
      R"({"blurb":"周淳结怨","keys":["周淳","毛太"],"relations":[{"a":"李宁","b":"周淳","rel":"故交"},{"a":"周淳","b":"毛太","rel":"仇怨"}],"thread":"周淳与毛太结怨"})"));
  mdl.replies.push_back(message::ai_message(
      R"({"blurb":"毛太师徒","keys":["毛太","邓明"],"relations":[{"a":"毛太","b":"邓明","rel":"师徒"}],"thread":"毛太师徒出场"})"));
}
}  // namespace

// 模型档 ingest 产 relations/thread；rel_net 按 a-b 排序键去重并记引用块。
TEST(WfLongdoc, ingest_extracts_relations_and_thread) {
  LongdocFixture fx;
  ScriptedLongdoc mdl;
  push_rel_replies(mdl);
  ComponentRegistry comps;
  comps.models["m"] = &mdl;
  CompileOptions copts;
  copts.components = &comps;
  auto st = fire(ingest_ir(), Json{{"longdoc_root", fx.corpus.string()},
                                   {"longdoc_index", fx.index_path().string()}}, copts);
  ASSERT_TRUE(st.state) << st.error;
  Json const idx = fx.read_index();
  ASSERT_EQ(idx["units"].size(), 3u);
  EXPECT_EQ(idx["units"][0]["thread"], "父女入川遇故交");
  ASSERT_EQ(idx["units"][0]["relations"].size(), 2u);
  // 聚合：李宁|周淳 去重为一条、两块引用
  bool found = false;
  for (auto const& r : idx["rel_net"])
    if (r["a"] == "李宁" && r["b"] == "周淳") {
      found = true;
      EXPECT_EQ(r["rel"], "故交");
      EXPECT_EQ(r["blocks"], Json::array({"a.txt", "b.txt"}));
    }
  EXPECT_TRUE(found) << "rel_net 应含去重后的 李宁|周淳";
  auto const& th = idx["threads"];
  ASSERT_EQ(th.size(), 3u);
  EXPECT_EQ(th[0]["id"], "a.txt");
}

// relation 节点：把 关系网+线索+问题 写进 system_context，供 model 作答。
TEST(WfLongdoc, relation_node_builds_context_from_net) {
  LongdocFixture fx;
  ScriptedLongdoc mdl;
  push_rel_replies(mdl);
  ComponentRegistry comps;
  comps.models["m"] = &mdl;
  CompileOptions copts;
  copts.components = &comps;
  auto st = fire(ingest_ir(), Json{{"longdoc_root", fx.corpus.string()},
                                   {"longdoc_index", fx.index_path().string()}}, copts);
  ASSERT_TRUE(st.state) << st.error;
  auto r = fire(relation_ir(),
                Json{{"longdoc_index", fx.index_path().string()},
                     {"longdoc_rel_query", "李宁与周淳什么关系？"}},
                copts);
  ASSERT_TRUE(r.state) << r.error;
  auto const& sc = (*r.state)["system_context"];
  ASSERT_TRUE(sc.is_array() && !sc.empty());
  std::string const text = sc[0]["content"][0].value("text", "");
  EXPECT_NE(text.find("李宁与周淳什么关系？"), std::string::npos);
  EXPECT_NE(text.find("【人物关系网】"), std::string::npos);
  EXPECT_NE(text.find("李宁 — 周淳：故交 [a.txt b.txt]"), std::string::npos);
  EXPECT_NE(text.find("【故事线索】"), std::string::npos);
  auto const& res = (*r.state)["results"]["rel"];
  EXPECT_EQ(res["relations"], 4);   // 李宁|英琼, 李宁|周淳, 毛太|周淳, 毛太|邓明
  EXPECT_EQ(res["threads"], 3);
}

// 无关系数据的索引（规则档）→ relation 节点 fail-closed。
TEST(WfLongdoc, relation_node_missing_data_fails_closed) {
  LongdocFixture fx;
  auto st = fire(ingest_ir(), Json{{"longdoc_root", fx.corpus.string()},
                                   {"longdoc_index", fx.index_path().string()}}, CompileOptions{});
  ASSERT_TRUE(st.state) << st.error;
  EXPECT_FALSE(fx.read_index().contains("rel_net"));
  auto r = fire(relation_ir(), Json{{"longdoc_index", fx.index_path().string()},
                                    {"longdoc_rel_query", "李宁与周淳？"}}, CompileOptions{});
  EXPECT_FALSE(r.state);
  EXPECT_NE(r.error.find("索引无人物关系"), std::string::npos);
}
