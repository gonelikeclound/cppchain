// cppchain —— src/graph/graph.cpp
#include "cppchain/graph/graph.hpp"

#include <algorithm>
#include <deque>
#include <string>
#include <utility>

namespace cppchain::graph {

namespace {

// 把待执行包队列转成供 Checkpointer::put 落盘的向量
std::vector<state::Packet> to_packets(std::deque<state::Packet> const& q) {
  return std::vector<state::Packet>(q.begin(), q.end());
}

}  // namespace

StateGraph& StateGraph::add_node(std::string name, NodeFn fn) {
  if (nodes_.find(name) == nodes_.end()) order_.push_back(name);
  nodes_[name] = std::move(fn);
  return *this;
}

StateGraph& StateGraph::add_edge(std::string from, std::string to) {
  static_edges_[std::move(from)] = std::move(to);
  return *this;
}

StateGraph& StateGraph::add_conditional_edges(std::string from, RouterFn router) {
  conditional_edges_[std::move(from)] = std::move(router);
  return *this;
}

StateGraph& StateGraph::set_entry_point(std::string name) {
  entry_ = std::move(name);
  return *this;
}

std::expected<CompiledGraph, GraphError> StateGraph::compile(state::Checkpointer* cp,
                                                             CompileOptions opts) const {
  auto err = [](std::string msg) {
    return std::unexpected(GraphError{GraphError::Kind::InvalidGraph, std::move(msg)});
  };

  if (nodes_.empty()) return err("graph has no nodes");
  if (entry_.empty()) return err("entry point not set");
  if (nodes_.find(entry_) == nodes_.end()) return err("entry point is not a node: " + entry_);

  // 静态边与条件边不可并存于同一节点（对齐 langchain 约束）
  for (auto const& [from, router] : conditional_edges_) {
    (void)router;
    if (nodes_.find(from) == nodes_.end()) return err("conditional edge from unknown node: " + from);
    if (static_edges_.find(from) != static_edges_.end())
      return err("node '" + from + "' has both static and conditional edges");
  }

  for (auto const& [from, to] : static_edges_) {
    if (nodes_.find(from) == nodes_.end()) return err("edge from unknown node: " + from);
    if (to != kEnd && nodes_.find(to) == nodes_.end()) return err("edge to unknown node: " + to);
  }

  return CompiledGraph(schema_, nodes_, static_edges_, conditional_edges_, entry_, cp, opts);
}

std::expected<std::vector<std::string>, GraphError> CompiledGraph::next_nodes(
    std::string const& node, Command const& cmd, Json const& state) const {
  // 1) Command.goto 优先（控制流原语）
  if (!cmd.goto_nodes.empty()) return cmd.goto_nodes;

  // 2) 静态边
  if (auto const it = static_edges_.find(node); it != static_edges_.end())
    return std::vector<std::string>{it->second};

  // 3) 条件边
  if (auto const it = conditional_edges_.find(node); it != conditional_edges_.end())
    return it->second(state);

  return std::vector<std::string>{};  // 无出边 → 结束
}

// 引擎事件投递：分配 seq + 一律经 sink_（本次 invoke 注入）——为空即静默
void CompiledGraph::emit(StreamEvent ev) {
  ev.seq = ++last_seq_;
  if (sink_) sink_(ev);
}

// 节点自报流式事件（模型 delta 等）。sink 空 / mode 无 Deltas / 无序列 → 静默。
void NodeContext::emit(std::string_view type, Json payload) {
  if (!sink || !(*sink) || (mode & EventMode::Deltas) == EventMode::Off || !seq) return;
  StreamEvent ev{.type = std::string(type), .node = node_name, .node_kind = "",
                 .step = step, .payload = std::move(payload), .ok = true, .status = "ok"};
  ev.seq = ++(*seq);  // 与引擎事件共用同一单调序列
  (*sink)(ev);
}

std::expected<Json, GraphError> CompiledGraph::invoke(Json const& input,
                                                      std::string const& thread_id,
                                                      InvokeOptions opts) {
  last_steps_ = 0;
  last_checkpoint_ = "";
  sink_ = std::move(opts.sink);
  mode_ = opts.mode;
  last_seq_ = 0;
  last_interrupt_ = {};

  // 收尾统一清理本次 invoke 的注入设施（不跨调用保留 sink）
  auto finish = [this] {
    sink_ = nullptr;
    mode_ = EventMode::Off;
  };

  Json state = input;
  std::deque<state::Packet> current;
  current.push_back(state::Packet{entry_});
  std::optional<std::string> parent;
  bool resuming_interrupt = false;
  std::string interrupt_node;

  // ① 恢复态：有快照 → 恢复到"该步边界继续"（input 被忽略）。
  //    默认取当前线程**最新**快照；fork 指定后取**那个线程的历史** checkpoint ——
  //    时间旅行分叉：从快照的 values + next 继续跑，新快照落到本次的 thread_id
  //    （原线程轨迹不污染），分叉点即新链的 parent。
  if (cp_ != nullptr && !thread_id.empty()) {
    std::string const src = opts.fork ? opts.fork->source_thread : thread_id;
    // fork.checkpoint_id 为空 = 该线程最新快照（文档语义）；空串不得包成 engaged 去精确查
    std::optional<std::string> const ckpt =
        (opts.fork && !opts.fork->checkpoint_id.empty())
            ? std::make_optional(opts.fork->checkpoint_id)
            : std::nullopt;
    auto snap = cp_->get_tuple(src, "", ckpt);
    if (opts.fork && !snap.has_value()) {
      // 分叉点找不到：fail-closed（不默默退回"最新 / 全新开跑"，防走错轨迹）
      finish();
      return std::unexpected(GraphError{
          GraphError::Kind::CheckpointNotFound,
          "fork 目标快照不存在: thread='" + opts.fork->source_thread +
              "' checkpoint='" + opts.fork->checkpoint_id + "'"});
    }
    if (snap.has_value()) {
      state = snap->values;
      current.assign(snap->next.begin(), snap->next.end());
      parent = snap->config.value("checkpoint_id", "");
      bool const interrupted = snap->metadata.value("interrupted", false);
      if (interrupted) {
        // 上次站在 interrupt 上：payload 从该快照所在线程的 writes 重建
        interrupt_node = snap->metadata.value("node", "");
        Json payload = Json::object();
        for (auto const& w : cp_->get_writes(src, "", *parent))
          if (w.channel == "interrupt") {
            payload = w.value;
            break;
          }
        last_interrupt_ = InterruptStatus{true, interrupt_node, std::move(payload)};
        if (!opts.resume) {   // 无答复：保持挂着，不动（只此时才重放 interrupt 事件告知挂着）
          emit(StreamEvent{.type = "interrupt", .node = interrupt_node, .node_kind = "",
                           .step = 0, .payload = last_interrupt_.payload, .ok = true,
                           .status = "interrupt"});
          finish();
          return state;
        }
        // 有 resume：续跑，不必重放旧的 interrupt（否则投影会出现幽灵 engine_interrupt 记录）
        resuming_interrupt = true;
      } else {
        if (opts.resume.has_value()) {
          finish();
          return std::unexpected(GraphError{GraphError::Kind::ResumeNotInterrupted,
                                            "thread '" + thread_id +
                                                "' 没有悬挂的中断，却收到了 resume 值"});
        }
        if (current.empty()) {  // 上次已跑完
          finish();
          return state;
        }
      }
    }
  }

  // ② 执行态：包队列顺序推进（一个 super-step 产生的所有目标都在队列里）
  while (!current.empty()) {
    if (last_steps_ >= opts_.max_steps) {
      finish();
      return std::unexpected(GraphError{GraphError::Kind::RecursionLimit,
                                        "exceeded max_steps=" + std::to_string(opts_.max_steps)});
    }

    state::Packet pkt = current.front();
    current.pop_front();
    std::string const node = std::move(pkt.node);
    if (node == kEnd) break;

    auto const it = nodes_.find(node);
    if (it == nodes_.end()) {
      finish();
      return std::unexpected(GraphError{GraphError::Kind::UnknownNode, "unknown node: " + node});
    }

    // 本包的输入视图：Send 包读自己的局部 state，普通包读全局 state
    Json node_input = pkt.state.has_value() ? *pkt.state : state;

    NodeContext ctx;
    ctx.node_name = node;
    ctx.thread_id = thread_id;
    ctx.step = static_cast<int>(last_steps_);
    ctx.sink = &sink_;
    ctx.mode = mode_;
    ctx.seq = &last_seq_;
    // resume 只注入**重跑包**（is_restart 标记），避免误给同节点的其它 Send 包
    if (resuming_interrupt && node == interrupt_node && pkt.is_restart)
      ctx.pending_resume = opts.resume;

    emit(StreamEvent{.type = "node_start", .node = node, .node_kind = "", .step = ctx.step,
                     .payload = Json::object(), .ok = true, .status = "ok"});

    Command cmd;
    try {
      cmd = it->second(node_input, ctx);
    } catch (InterruptSignal const& sig) {
      // 悬挂需要 checkpoint 承载；无 cp / 无 thread_id 时 interrupt 不可用（对齐
      // LangGraph "interrupt 需要 checkpointer"——否则挂起来没地方存、也没法恢复）
      if (cp_ == nullptr || thread_id.empty()) {
        finish();
        return std::unexpected(
            GraphError{GraphError::Kind::InterruptUnavailable,
                       "interrupt() 需要 checkpointer 且 thread_id 非空（否则无法暂停/恢复）"});
      }
      // 首次悬挂：节点无 update 落盘 → 快照取节点入口的完整 state；
      // next = 剩余包 + 重跑包（放最后，**保留本包的局部输入视图**）；payload 随 writes 落盘
      last_interrupt_ = InterruptStatus{true, node, sig.payload()};
      // 重跑包打 is_restart 标记：resume 只注入这个包（同节点扇入的其它包不得误拿）
      current.push_back(state::Packet{node, pkt.state, /*is_restart=*/true});
      Json meta = {{"step", last_steps_}, {"node", node}, {"interrupted", true}};
      last_checkpoint_ = cp_->put(thread_id, "", parent, state, to_packets(current), meta);
      cp_->put_writes(thread_id, "", last_checkpoint_,
                      {state::WriteRecord{node, "interrupt", last_interrupt_.payload}});
      parent = last_checkpoint_;
      emit(StreamEvent{.type = "interrupt", .node = node, .node_kind = "", .step = ctx.step,
                       .payload = last_interrupt_.payload, .ok = true, .status = "interrupt"});
      finish();
      return state;  // 以"挂着"返回完整 state；调用方用 last_interrupt() 区分
    } catch (std::exception const& e) {
      emit(StreamEvent{.type = "node_end", .node = node, .node_kind = "", .step = ctx.step,
                       .payload = Json{{"error", e.what()}}, .ok = false, .status = "error"});
      finish();
      return std::unexpected(GraphError{GraphError::Kind::NodeError,
                                        "node '" + node + "' threw: " + e.what()});
    }

    state = schema_.apply(state, cmd.update);  // 包结果经 per-key reducer 归约回主 state
    emit(StreamEvent{.type = "node_end", .node = node, .node_kind = "", .step = ctx.step,
                     .payload = cmd.update, .ok = true, .status = "ok"});
    if ((mode_ & EventMode::Snapshot) != EventMode::Off)
      emit(StreamEvent{.type = "values", .node = "", .node_kind = "", .step = ctx.step,
                       .payload = state, .ok = true, .status = "ok"});

    // ③ 路由态：goto > sends > 静态边 > 条件边
    // 语义：本包产出的目标**追加**到队尾 → 一个 super-step 产生的所有包都先跑完
    //（同一批内 FIFO 顺序执行），而非替换掉尚未执行的剩余包。
    std::vector<state::Packet> produced;
    if (!cmd.goto_nodes.empty()) {
      for (auto const& n : cmd.goto_nodes)
        if (n != std::string(kEnd)) produced.emplace_back(n);  // goto 里的 END = 在此结束
    } else if (!cmd.sends.empty()) {
      for (auto const& s : cmd.sends) {
        if (nodes_.find(s.node) == nodes_.end()) {
          finish();
          return std::unexpected(GraphError{GraphError::Kind::UnknownNode,
                                            "send target unknown node: " + s.node});
        }
        produced.emplace_back(s.node, std::optional<Json>{s.state});
      }
    } else {
      auto next = next_nodes(node, cmd, state);
      if (!next) {
        finish();
        return std::unexpected(next.error());
      }
      // END 不是节点：先剔除，避免污染快照（恢复语义 = "待执行的节点"）
      next->erase(std::remove(next->begin(), next->end(), std::string(kEnd)), next->end());
      for (auto const& n : *next) produced.emplace_back(n);
    }
    for (auto& p : produced) current.push_back(std::move(p));

    // 每步落一个快照：values + 剩余全部待执行包（恢复语义 = 回到该步边界继续）
    if (cp_ != nullptr && !thread_id.empty()) {
      Json meta = {{"step", last_steps_}, {"node", node}};
      last_checkpoint_ = cp_->put(thread_id, "", parent, state, to_packets(current), meta);
      parent = last_checkpoint_;
    }

    ++last_steps_;
  }

  last_interrupt_ = {};  // 正常跑完：不再挂着（resume 已消费）
  finish();
  return state;
}

// 便捷收集：跑一遍并把事件全收下来（供测试/demo 观察；唯一执行通道仍是 invoke）
std::vector<StreamEvent> CompiledGraph::stream(Json const& input, std::string const& thread_id) {
  std::vector<StreamEvent> out;
  EventSink collector = [&out](StreamEvent const& e) { out.push_back(e); };
  (void)invoke(input, thread_id, InvokeOptions{collector, EventMode::All, std::nullopt});
  return out;
}

}  // namespace cppchain::graph