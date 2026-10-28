// cppchain —— workflow/file_route.hpp
// problems/02 追加 S6：文件取数路由（规则判定 + 升级阶梯 + 证据）。
//
// 输入：最后一条用户提问 + 已注册的文件工具（file_grep / file_extract / file_sample /
//       list_dir / file_read）；输出：注入给模型的 system_context 片段 + 可审计的路由帧。
// 设计要点：
//   · 判定只用**确定性信号**（结构路径 / 引用串 / 代码字面 / 全局词 / 完整性词），零模型成本；
//   · 失败升级：extract 未命中 → grep → sample；grep 未命中 → sample；采样永远带"不保证完整"声明；
//   · 结果受 budget_chars 约束；超预算部分标注截断（不静默丢）。
#pragma once

#include <string>
#include <vector>

#include "cppchain/message/message.hpp"
#include "cppchain/model/embeddings.hpp"
#include "cppchain/tool/registry.hpp"

namespace cppchain::workflow {

struct FileRouteOptions {
  long budget_chars = 2000;  // 注入 system_context 的素材总预算（字节）
  long max_files = 5;        // 候选文件上限（list_dir 截断）
  // ⑤ BM25 / ⑥ 向量升级（grep/extract 未命中时的兜底检索）。
  model::Embeddings* embedder = nullptr;  // 可选：配置了 embedding 组件才走向量臂
  double vector_weight = 0.5;             // 混合权重（0 = 纯 BM25，1 = 纯向量）
  long retrieve_max_bytes = 200000;       // 每文件最多读入的字节（分页 file_read）
  std::size_t retrieve_top_k = 3;         // 注入片段的条数
};

struct FileRouteOutcome {
  std::string route = "none";           // grep / extract / sample / none
  nlohmann::json attempts = nlohmann::json::array();    // [{tool,detail,hit,bytes}]
  nlohmann::json candidates = nlohmann::json::array();  // 候选文件名
  std::vector<message::Message> context;                // 待注入的 system 消息
  bool budget_hit = false;
};

// 对一个问题执行一次路由（纯 IO 只读；工具缺失时 fail-open → route="none"）。
[[nodiscard]] FileRouteOutcome route_file_question(tool::ToolRegistry const& reg,
                                                   std::string const& question,
                                                   FileRouteOptions opts = {});

}  // namespace cppchain::workflow
