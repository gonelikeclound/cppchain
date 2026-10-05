// ⑤b（A1）用的"全好"库：枚举到的每个节点都能干净加载（与 node_demo_plugin 故意
// 掺坏 creator 形成对比）。导出两个简单节点 blink / nudge —— 每个把名字追加成一条消息。
//
// A1 的 import 省略 nodes 走枚举时，会在**整库信任**下加载这里的所有创建者；
// 若库里混着坏 creator（如 node_demo_plugin 的 misnamed/ancient），省略加载会整体失败 ——
// 那种库只能显式列名，这也是"库是信任单元"的本意。
#include "cppchain/workflow/plugin_node.hpp"

#include <string>
#include <vector>

#include "cppchain/graph/graph.hpp"
#include "cppchain/message/serializer.hpp"
#include "cppchain/workflow/refs.hpp"  // kMessagesKey

using namespace cppchain;
using namespace cppchain::workflow;

namespace {

class EchoNode final : public DynamicNode {
 public:
  explicit EchoNode(std::string name) : name_(std::move(name)) {}
  bool matches(Json const& j) const override {
    return j.is_object() && j.value("kind", "") == name_;
  }
  void parse(NodeIR&, Json const&) const override {}
  void write(Json& j, NodeIR const& n) const override {
    j["kind"] = name_;
    if (n.raw.contains("next")) j["next"] = n.raw["next"];
  }
  std::string describe(NodeIR const& n) const override {
    return name_ + " → " + n.raw.value("next", "");
  }
  std::vector<NodeType::Edge> edges(NodeIR const& n) const override {
    return {{n.raw.value("next", ""), "next", true}};
  }
  std::vector<std::string> ref_texts(NodeIR const&) const override { return {}; }
  void validate(NodeIR const& n, NodeType::ValidateContext const&,
                NodeType::ErrorSink const& err) const override {
    if (n.raw.value("next", "").empty())
      err("EN1", name_ + " 节点 '" + n.id + "' 缺少 next（成功出口）");
  }
  void compile(NodeIR const& n, NodeType::CompileContext& cc) const override {
    std::string const chunk = name_;
    // 复用宿主图 API（LoggingStateGraph 自动给它埋结构事件）；对 cc.graph.* / message::*
    // 的引用保持未定义，dlopen 时由宿主 --export-dynamic 解析
    cc.graph.add_node(n.id, [chunk](Json const&, cppchain::graph::NodeContext&) {
      return cppchain::graph::Command::with_update(
          Json{{std::string(kMessagesKey),
                Json::array({cppchain::message::human_message(chunk)})}});
    });
    cc.graph.add_edge(n.id, cc.resolve(n.raw.value("next", "")));
    ++cc.registered;
  }

 private:
  std::string name_;
};

}  // namespace

// 每个 creator 自带静态 desc + 静态节点（node 由库持有、进程期生命周期）。
extern "C" CPPCHAIN_NODE_EXPORT cppchain_node_descriptor const* graph_node_blink_creator(void*) {
  static EchoNode node("blink");
  static cppchain_node_descriptor desc{};
  desc = cppchain_node_descriptor{};
  desc.name = "blink";  // 字面量，进程期稳定
  desc.version = kNodeApiVersion;
  desc.context = nullptr;
  desc.create = [](void*) -> void* { return &node; };   // 静态 node：无捕获
  desc.destroy = [](void*, void*) {};
  return &desc;
}

extern "C" CPPCHAIN_NODE_EXPORT cppchain_node_descriptor const* graph_node_nudge_creator(void*) {
  static EchoNode node("nudge");
  static cppchain_node_descriptor desc{};
  desc = cppchain_node_descriptor{};
  desc.name = "nudge";
  desc.version = kNodeApiVersion;
  desc.context = nullptr;
  desc.create = [](void*) -> void* { return &node; };
  desc.destroy = [](void*, void*) {};
  return &desc;
}