// ⑤ 动态库节点示例（docs/02-arch-design/12 §5）：一个 "sentiment" 节点。
//
// 这个 .so 同时示范三件事：
//   1) 好路径：graph_node_sentiment_creator —— 名实相符、version 匹配 → 宿主注册成功
//   2) 改名漏检自校验：graph_node_misnamed_creator —— 符号叫 misnamed，descriptor.name 却是
//      "sentiment" → 宿主的"符号名与 descriptor.name 一致"校验会拒（§5.1 name 字段的存在理由）
//   3) ABI 版本校验：graph_node_ancient_creator —— version=0 ≠ kNodeApiVersion → 宿主拒
//
// 符号可见性（§5.4）：必须 CPPCHAIN_NODE_EXPORT（CMake 默认 -fvisibility=hidden 时不导出）。
#include "cppchain/workflow/plugin_node.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "cppchain/graph/graph.hpp"
#include "cppchain/message/serializer.hpp"
#include "cppchain/workflow/refs.hpp"  // kMessagesKey

using namespace cppchain;
using namespace cppchain::workflow;

namespace {

using cppchain::workflow::DynamicNode;
using cppchain::workflow::Json;  // 见 plugin_node.hpp 转来的 nlohmann
using cppchain::workflow::NodeIR;
using cppchain::workflow::NodeType;
namespace wf = cppchain::workflow;

// sentiment 节点：JSON 形状 { id, kind:"sentiment", score:int≥0, query?:string, next:string }。
// 字段直接读 NodeIR::raw（原 JSON；这种"扁平共用槽位"正是 §9 Q1 选 (a) 的本意）。
class SentimentNode final : public DynamicNode {
 public:
  bool matches(Json const& j) const override {
    return j.is_object() && j.value("kind", "") == "sentiment";
  }
  void parse(NodeIR&, Json const&) const override {}  // 字段在 raw 里，无需挪动
  void write(Json& j, NodeIR const& n) const override {
    Json const& raw = n.raw;
    j["kind"] = "sentiment";
    if (raw.contains("score")) j["score"] = raw["score"];
    if (raw.contains("query")) j["query"] = raw["query"];
    if (raw.contains("next")) j["next"] = raw["next"];
  }
  std::string describe(NodeIR const& n) const override {
    return "sentiment score=" + std::to_string(n.raw.value("score", 0)) + " → " +
           n.raw.value("next", "");
  }
  std::vector<NodeType::Edge> edges(NodeIR const& n) const override {
    return {{n.raw.value("next", ""), "next", true}};  // 前向边，喂给 VG1/VG2/VG7/VG8
  }
  std::vector<std::string> ref_texts(NodeIR const& n) const override {
    if (n.raw.contains("query") && n.raw["query"].is_string()) return {n.raw["query"].get<std::string>()};
    return {};  // query 可含 $/@config 引用 → VR1/VR4 会检查它
  }
  void validate(NodeIR const& n, NodeType::ValidateContext const&,
                NodeType::ErrorSink const& err) const override {
    if (n.raw.contains("score")) {
      if (!n.raw["score"].is_number_integer())
        err("PK1", "sentiment 节点 '" + n.id + "' 的 score 必须是整数");
      else if (n.raw["score"].get<int>() < 0)
        err("PK1", "sentiment 节点 '" + n.id + "' 的 score 不能为负");
    }
    if (n.raw.value("next", "").empty())
      err("PK2", "sentiment 节点 '" + n.id + "' 缺少 next（成功出口）");
  }
  void compile(NodeIR const& n, NodeType::CompileContext& cc) const override {
    int const score = n.raw.value("score", 0);
    std::string const next = n.raw.value("next", "");
    // 复用宿主图 API：编译期 add_node/add_edge（LoggingStateGraph 会自动给它埋结构事件）
    cc.graph.add_node(n.id, [score](Json const&, cppchain::graph::NodeContext&) {
      return cppchain::graph::Command::with_update(
          Json{{std::string(wf::kMessagesKey),
                Json::array({cppchain::message::human_message(
                    "sentiment scored " + std::to_string(score))})}});
    });
    cc.graph.add_edge(n.id, cc.resolve(next));
    ++cc.registered;
  }
};

cppchain_node_descriptor const* make_desc(char const* name, int version) {
  static SentimentNode node;
  static cppchain_node_descriptor desc{};
  desc = cppchain_node_descriptor{};
  desc.name = name;
  desc.version = version;
  desc.context = nullptr;
  desc.create = [](void*) -> void* { return &node; };
  desc.destroy = [](void*, void*) {};  // node 是静态的，无需释放
  return &desc;
}

}  // namespace

// 好路径：名实相符
extern "C" CPPCHAIN_NODE_EXPORT cppchain_node_descriptor const* graph_node_sentiment_creator(void*) {
  return make_desc("sentiment", kNodeApiVersion);
}

// 改名漏改：符号 misnamed，descriptor.name 却是 "sentiment" → 宿主按名加载时立刻发现
extern "C" CPPCHAIN_NODE_EXPORT cppchain_node_descriptor const* graph_node_misnamed_creator(void*) {
  return make_desc("sentiment", kNodeApiVersion);  // 故意不一致
}

// ABI 版本过老：version=0 ≠ kNodeApiVersion → 宿主拒（拒绝理由正是 §5.1 version 字段）
extern "C" CPPCHAIN_NODE_EXPORT cppchain_node_descriptor const* graph_node_ancient_creator(void*) {
  return make_desc("ancient", 0);
}