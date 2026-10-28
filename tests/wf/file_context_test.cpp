// tests/wf —— problems/02 追加 S6：file_context 文件取数路由。
// 覆盖：结构路径 → extract、代码字面 → grep、全局词 → sample、失败升级（extract→grep→sample）、
// 预算截断、默认关零回归、无信号 noop。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "cppchain/message/message.hpp"
#include "cppchain/message/serializer.hpp"
#include "cppchain/state/checkpointer.hpp"
#include "cppchain/model/embeddings.hpp"
#include "cppchain/workflow/file_route.hpp"
#include "cppchain/tool/file_tools.hpp"
#include "cppchain/tool/tool.hpp"
#include "cppchain/workflow/compiler.hpp"
#include "cppchain/workflow/components.hpp"
#include "cppchain/workflow/ir.hpp"
#include "cppchain/workflow/refs.hpp"
#include "cppchain/workflow/session_log.hpp"
#include "cppchain/workflow/validator.hpp"

using namespace cppchain;
using namespace cppchain::workflow;

using Json = nlohmann::json;

namespace {

Json route_ir() {
  // recall(load_history) → ctx(file_context) → _turn_end：与真实 chat 同形，
  // 用户提问经会话日志 replay 进 messages（引擎不把 config 里的 messages 当 state）。
  return Json{{"id", "wf_route"},
              {"nodes", Json::array({
                  Json{{"id", "recall"}, {"kind", "load_history"},
                       {"path", "@config.workspace"}, {"session", "@config.session"},
                       {"next", "ctx"}},
                  Json{{"id", "ctx"}, {"kind", "file_context"}, {"next", "_turn_end"}},
              })}};
}

struct Workspace {
  std::filesystem::path dir;
  Workspace()
      : dir(std::filesystem::temp_directory_path() /
            ("wf_route_" + std::to_string(static_cast<unsigned>(std::random_device{}())))) {
    std::filesystem::create_directories(dir);
  }
  ~Workspace() { std::filesystem::remove_all(dir); }
};

void write_text(std::filesystem::path const& p, std::string const& s) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << s;
}

struct Fixture {
  Workspace ws;
  tool::ToolRegistry registry;
  ComponentRegistry components;
  state::MemoryCheckpointer cp;

  Fixture() {
    for (auto& t : tool::make_file_tools(tool::FileToolsConfig{ws.dir})) registry.add(std::move(t));
  }

  std::expected<Json, WorkflowError> run(std::string const& question, Json config) {
    // 本轮输入写进会话日志（与 wfr 一致），load_history 再 replay 进 messages
    {
      SessionLog log(session_path(ws.dir.string(), "default"));
      log.engine_enter("r-1", "__input__", "input");
      log.engine_exit("r-1", "__input__", "input", true, "ok", 0,
                      Json{{"messages", Json::array({Json(message::human_message(question))})}});
    }
    auto parsed = WorkflowIR::parse(route_ir());
    if (!parsed)
      return std::unexpected(WorkflowError{WorkflowError::Kind::Invalid, parsed.error()});
    ValidatorOptions vopts;
    vopts.registry = &registry;
    vopts.components = &components;
    auto const v = validate(*parsed, vopts);
    if (!v.ok()) return std::unexpected(WorkflowError{WorkflowError::Kind::Invalid, v.describe()});
    CompileOptions c;
    c.components = &components;
    auto wf = compile_workflow(*parsed, registry, &cp, c);
    if (!wf) return std::unexpected(wf.error());
    Json state_in = std::move(config);
    state_in["workspace"] = ws.dir.string();
    state_in["session"] = "default";
    FireOptions fo;
    fo.thread_id = "t1";
    return fire_once(*wf, state_in, fo);
  }
};

std::string context_text(Json const& st) {
  auto const it = st.find("system_context");
  if (it == st.end() || !it->is_array()) return "";
  std::string out;
  for (Json const& m : *it)
    if (m.contains("content") && m["content"].is_array())
      for (Json const& blk : m["content"])
        if (blk.is_object() && blk.contains("text")) out += blk["text"].get<std::string>();
  return out;
}

Json const& route_frame(Json const& st) { return st.at("results").at("ctx"); }

}  // namespace

TEST(WfFileContext, disabled_by_default) {
  Fixture f;
  write_text(f.ws.dir / "a.txt", "hello");
  auto st = f.run("hello", Json::object());
  ASSERT_TRUE(st) << st.error().message;
  EXPECT_FALSE(st->contains("system_context"));
  EXPECT_FALSE(st->contains("results") && st->at("results").contains("ctx"));  // 默认关：零回归
}

TEST(WfFileContext, literal_routes_to_grep) {
  Fixture f;
  std::string log;
  for (int i = 0; i < 3000; ++i)
    log += (i == 2000) ? "2026-10-27 ERROR code=0x9F3A handler=payment\n"
                       : "2026-10-27 INFO request " + std::to_string(i) + "\n";
  write_text(f.ws.dir / "app.log", log);

  auto st = f.run("日志里 code=0x9F3A 是什么？",
                  Json{{"file_route", Json{{"enabled", true}}}});
  ASSERT_TRUE(st) << st.error().message;
  EXPECT_EQ(route_frame(*st).at("route").get<std::string>(), "grep");
  EXPECT_NE(context_text(*st).find("0x9F3A"), std::string::npos);
  auto const& a = route_frame(*st).at("attempts");
  ASSERT_FALSE(a.empty());
  EXPECT_EQ(a[0].at("tool").get<std::string>(), "file_grep");
  EXPECT_TRUE(a[0].at("hit").get<bool>());
}

TEST(WfFileContext, json_pointer_routes_to_extract) {
  Fixture f;
  Json doc{{"services", Json::array()}};
  for (int i = 0; i < 50; ++i) doc["services"].push_back({{"ports", {8000 + i, 9000 + i}}});
  doc["services"][3] = {{"ports", {8080, 8443}}};
  write_text(f.ws.dir / "config.json", doc.dump(2));

  auto st = f.run("config.json 的 /services/3/ports/1 是多少？",
                  Json{{"file_route", Json{{"enabled", true}}}});
  ASSERT_TRUE(st) << st.error().message;
  EXPECT_EQ(route_frame(*st).at("route").get<std::string>(), "extract");
  EXPECT_NE(context_text(*st).find("8443"), std::string::npos);
  EXPECT_EQ(route_frame(*st).at("attempts")[0].at("tool").get<std::string>(), "file_extract");
}

TEST(WfFileContext, extract_miss_escalates_to_grep_then_sample) {
  Fixture f;
  Json doc{{"a", 1}};
  write_text(f.ws.dir / "config.json", doc.dump(2));
  write_text(f.ws.dir / "other.txt", "sample material for the fallback\n");

  auto st = f.run("config.json 的 /nope/x 是多少？",
                  Json{{"file_route", Json{{"enabled", true}}}});
  ASSERT_TRUE(st) << st.error().message;
  EXPECT_EQ(route_frame(*st).at("route").get<std::string>(), "sample");
  auto const& a = route_frame(*st).at("attempts");
  ASSERT_GE(a.size(), 4u);
  EXPECT_EQ(a[0].at("tool").get<std::string>(), "file_extract");
  EXPECT_FALSE(a[0].at("hit").get<bool>());
  EXPECT_EQ(a[1].at("tool").get<std::string>(), "file_grep");
  EXPECT_FALSE(a[1].at("hit").get<bool>());
  EXPECT_EQ(a[2].at("tool").get<std::string>(), "retrieve");  // BM25/向量升级
  EXPECT_FALSE(a[2].at("hit").get<bool>());
  EXPECT_EQ(a[3].at("tool").get<std::string>(), "file_sample");
  EXPECT_TRUE(a[3].at("hit").get<bool>());
}

TEST(WfFileContext, global_word_routes_to_sample) {
  Fixture f;
  std::string novel;
  for (int i = 1; i <= 40; ++i) {
    novel += "第" + std::to_string(i) + "章 江湖夜雨\n";
    for (int k = 0; k < 12; ++k) novel += "山中夜雨，剑气纵横（第" + std::to_string(i) + "章）。\n";
  }
  novel += "全书完。\n";
  write_text(f.ws.dir / "novel.txt", novel);

  auto st = f.run("总结一下 novel.txt", Json{{"file_route", Json{{"enabled", true}}}});
  ASSERT_TRUE(st) << st.error().message;
  EXPECT_EQ(route_frame(*st).at("route").get<std::string>(), "sample");
  EXPECT_NE(context_text(*st).find("不保证覆盖全文"), std::string::npos);
}

TEST(WfFileContext, quoted_literal_in_completeness_question_routes_to_grep) {
  Fixture f;
  std::string novel = "第1章 开端\n李宁入川。\n";
  novel += "第12章 相遇\n李英琼提剑走入雨里。\n";
  novel += std::string(2000, 'x') + "\n";
  write_text(f.ws.dir / "novel.txt", novel);

  auto st = f.run("novel.txt 里有没有提到「李英琼」？",
                  Json{{"file_route", Json{{"enabled", true}}}});
  ASSERT_TRUE(st) << st.error().message;
  EXPECT_EQ(route_frame(*st).at("route").get<std::string>(), "grep");
  EXPECT_NE(context_text(*st).find("李英琼"), std::string::npos);
}

TEST(WfFileContext, budget_truncates_injected_context) {
  Fixture f;
  std::string log;
  for (int i = 0; i < 200; ++i) log += "0x9F3A match line " + std::to_string(i) + "\n";
  write_text(f.ws.dir / "app.log", log);

  auto st = f.run("code=0x9F3A 是什么？",
                  Json{{"file_route", Json{{"enabled", true}, {"budget_chars", 60}}}});
  ASSERT_TRUE(st) << st.error().message;
  EXPECT_TRUE(route_frame(*st).at("budget_hit").get<bool>());
  EXPECT_NE(context_text(*st).find("路由预算已满"), std::string::npos);
}

TEST(WfFileContext, no_signal_is_noop) {
  Fixture f;
  write_text(f.ws.dir / "a.txt", "hello");
  auto st = f.run("你好呀", Json{{"file_route", Json{{"enabled", true}}}});
  ASSERT_TRUE(st) << st.error().message;
  EXPECT_EQ(route_frame(*st).at("route").get<std::string>(), "none");
  EXPECT_EQ(route_frame(*st).at("action").get<std::string>(), "noop");
  EXPECT_FALSE(st->contains("system_context"));
}


// ⑤ BM25：字面 grep 未命中（"召回率" vs 正文"召回指标"）→ 检索升级命中
TEST(WfFileContext, bm25_escalates_when_grep_misses) {
  Fixture f;
  write_text(f.ws.dir / "notes.txt", "背景说明。\n召回指标：0.83，精确率 0.91。\n其他内容。\n");
  auto st = f.run("「召回率」是多少？", Json{{"file_route", Json{{"enabled", true}}}});
  ASSERT_TRUE(st) << st.error().message;
  EXPECT_EQ(route_frame(*st).at("route").get<std::string>(), "retrieve");
  EXPECT_NE(context_text(*st).find("0.83"), std::string::npos);
  auto const& a = route_frame(*st).at("attempts");
  ASSERT_FALSE(a.empty());
  EXPECT_EQ(a.back().at("tool").get<std::string>(), "retrieve");
  EXPECT_TRUE(a.back().at("hit").get<bool>());
}

// ⑥ 向量：query 与正文无词面重叠（BM25=0），向量臂命中 → retrieve
namespace {
class FakeEmbedder final : public model::Embeddings {
 public:
  std::expected<std::vector<model::Embedding>, model::ModelError> embed(
      std::span<std::string const> texts) override {
    std::vector<model::Embedding> out;
    for (std::size_t i = 0; i < texts.size(); ++i) out.push_back(model::Embedding{1.0f, 0.0f});
    return out;
  }
  std::size_t dimension() const override { return 2; }
  std::size_t max_batch_size() const override { return 64; }
};
}  // namespace

TEST(WfFileContext, vector_arm_hits_without_token_overlap) {
  Fixture f;
  write_text(f.ws.dir / "notes.txt", "完全不相关的正文，没有任何共同词。\n");
  FakeEmbedder emb;
  FileRouteOptions opts;
  opts.embedder = &emb;
  opts.vector_weight = 1.0;
  auto const out = route_file_question(f.registry, "「XYZQUERY」是什么？", opts);
  EXPECT_EQ(out.route, "retrieve");
  EXPECT_FALSE(out.context.empty());
}
