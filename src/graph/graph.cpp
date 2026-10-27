// cppchain —— src/graph/graph.cpp
#include "cppchain/graph/graph.hpp"

#include <algorithm>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
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

// 引擎事件投递：分配 seq + 一律经 sink_（本次 invoke 注入）——为空即静默。
// 线程安全：并行波 worker 的节点自报事件（NodeContext::emit）也走这里的锁，
// 保证 seq 单调且 sink 不被并发撕裂。
void CompiledGraph::emit(StreamEvent ev) {
  std::lock_guard<std::mutex> lk(*emit_mu_);
  ev.seq = ++last_seq_;
  if (sink_) sink_(ev);
}

// 节点自报流式事件（模型 delta 等）。sink 空 / mode 无 Deltas / 无序列 → 静默。
// 调用线程：普通路径为主线程；并行波 worker 内也安全（emit_mu 互斥 + seq 锁内分配）。
void NodeContext::emit(std::string_view type, Json payload) {
  if (!sink || !(*sink) || (mode & EventMode::Deltas) == EventMode::Off || !seq) return;
  StreamEvent ev{.type = std::string(type), .node = node_name, .node_kind = "",
                 .step = step, .payload = std::move(payload), .ok = true, .status = "ok"};
  if (emit_mu) {
    std::lock_guard<std::mutex> lk(*emit_mu);
    ev.seq = ++(*seq);  // 与引擎事件共用同一单调序列
    (*sink)(ev);
  } else {
    ev.seq = ++(*seq);
    (*sink)(ev);
  }
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

  // 全主体包进 try：无论异常还是 return，注入设施都在收尾被清掉。
  //   - return 路径各自显式 finish()；
  //   - throw 路径（非 std 异常的节点 / schema.apply / checkpoint 落盘 / 宿主 sink）由
  //     catch(...) 兜底 finish() 后继续传播 —— 否则 sink_/mode_ 残留到下次 invoke（自愈但
  //     调用方若 catch 后立刻读 getter 会拿到脏态）。
  try {

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

    // ⊕ 并行波（wfr-㊾ 批②）：max_parallel>1 时，队列顶部**连续的一段 Send 包**（size>1）
    //   作为一个不可分割的波在 worker 线程上并发执行。串行默认（=1）不触发，逐包 FIFO
    //   （既有行为一字不改）。波 = 屏障：join 后才继续；join 后**按声明序**逐键归约回主
    //   state（键独立则与串行最终一致）；冲突护栏拦截同键并发写默认键；波落一个快照；
    //   成员自身的路由产物按成员序追加入队。is_restart（中断重跑包）不参与成波。
    if (opts_.max_parallel > 1) {
      std::size_t w = 0;
      for (auto const& p : current)
        if (p.state.has_value() && !p.is_restart) ++w;
        else break;
      if (w > 1) {
        std::vector<state::Packet> members(current.begin(), current.begin() + w);
        current.erase(current.begin(), current.begin() + w);

        for (auto const& m : members)
          if (nodes_.find(m.node) == nodes_.end()) {
            finish();
            return std::unexpected(GraphError{GraphError::Kind::UnknownNode,
                                              "send target unknown node: " + m.node});
          }
        // 波内成员各计一步：本波会超界 → 此时节点自身收尾已完成、快照已落（可恢复），
        // 与串行「跑完该步后下一圈才报」对等。
        if (last_steps_ + w > opts_.max_steps) {
          finish();
          return std::unexpected(GraphError{GraphError::Kind::RecursionLimit,
                                            "exceeded max_steps=" +
                                                std::to_string(opts_.max_steps) +
                                                " (parallel wave)"});
        }

        std::size_t const base = last_steps_;
        std::vector<Json> updates(w, Json::object());
        std::vector<std::vector<state::Packet>> produced(w);
        std::vector<std::optional<std::string>> err_kind(w, std::nullopt);
        std::vector<std::string> err_msg(w);

        // 按序发射 node_start：主线程先行（结构事件顺序确定 → 会话日志重放不分叉）
        for (std::size_t k = 0; k < w; ++k)
          emit(StreamEvent{.type = "node_start", .node = members[k].node, .node_kind = "",
                           .step = static_cast<int>(base + k), .payload = Json::object(),
                           .ok = true, .status = "ok"});

        {
          std::vector<std::thread> threads;
          threads.reserve(w);
          for (std::size_t k = 0; k < w; ++k) {
            threads.emplace_back([this, k, &members, base, thread_id, &state, &updates,
                                  &produced, &err_kind, &err_msg] {
              NodeContext wctx;
              wctx.node_name = members[k].node;
              wctx.thread_id = thread_id;
              wctx.step = static_cast<int>(base + k);
              wctx.sink = &sink_;
              wctx.mode = mode_;
              wctx.seq = &last_seq_;
              wctx.emit_mu = emit_mu_.get();  // delta 直发：seq 分配 + sink 投递在锁内
              wctx.shared_tools = opts_.shared_tools;  // wfr-㊹：并行波成员同样注入共享工具注册表
              // resume 不注入并行波成员（波内 interrupt 被拒，见下）
              Command mc;
              try {
                mc = nodes_.at(members[k].node)(
                    members[k].state.has_value() ? *members[k].state : state, wctx);
              } catch (InterruptSignal const&) {
                // 并行波内成员 interrupt：无法在波中途落「剩余包+重跑包」快照（其它成员仍在
                // 跑、波不可分割）→ fail-closed 明确拒绝，不默默串行化。
                err_kind[k] = "interrupt_unavailable";
                err_msg[k] = "并行波成员 " + members[k].node +
                             " 调用了 interrupt（Send 并行不支持）——请设 max_parallel=1"
                             " 或把该步骤移出并行波";
                return;
              } catch (std::exception const& e) {
                err_kind[k] = "node_error";
                err_msg[k] = "node '" + members[k].node + "' threw: " + e.what();
                return;
              } catch (...) {
                err_kind[k] = "node_error";
                err_msg[k] = "node '" + members[k].node + "' threw: non-std exception";
                return;
              }
              updates[k] = std::move(mc.update);
              // 成员自身的路由（goto > sends > 静态/条件边），语义与串行路径一致；
              // next_nodes 读主 state —— 波期间主线程不写 state，并发只读安全。
              if (!mc.goto_nodes.empty()) {
                for (auto const& n : mc.goto_nodes)
                  if (n != std::string(kEnd)) produced[k].emplace_back(n);
              } else if (!mc.sends.empty()) {
                for (auto const& ss : mc.sends) {
                  if (nodes_.find(ss.node) == nodes_.end()) {
                    err_kind[k] = "unknown_node";
                    err_msg[k] = "send target unknown node: " + ss.node;
                    return;
                  }
                  produced[k].emplace_back(ss.node, std::optional<Json>{ss.state});
                }
              } else {
                auto next = next_nodes(members[k].node, mc, state);
                if (!next) {
                  err_kind[k] = "invalid_graph";
                  err_msg[k] = next.error().message;
                  return;
                }
                for (auto const& n : *next)
                  if (n != std::string(kEnd)) produced[k].emplace_back(n);
              }
            });
          }
          for (auto& t : threads) t.join();
        }

        // 按序报首个成员错误（确定性）：interrupt_unavailable / unknown_node / node_error / …
        for (std::size_t k = 0; k < w; ++k) {
          if (!err_kind[k]) continue;
          GraphError::Kind kind = GraphError::Kind::NodeError;
          if (*err_kind[k] == "interrupt_unavailable") kind = GraphError::Kind::InterruptUnavailable;
          if (*err_kind[k] == "unknown_node") kind = GraphError::Kind::UnknownNode;
          if (*err_kind[k] == "invalid_graph") kind = GraphError::Kind::InvalidGraph;
          finish();
          return std::unexpected(GraphError{kind, err_msg[k]});
        }

        // 冲突护栏：波内 ≥2 个成员写同一键、且该键为默认 overwrite（无自定义 reducer）
        // → ConcurrentWriteConflict，fail-closed（不默默后写覆盖前写）。累积类 reducer
        // （messages/results 等）按声明序折叠合法。逼出正确用法：并行步骤各写各的键/文件。
        {
          std::unordered_map<std::string, std::size_t> first_writer;  // key → 首个写者 index
          std::vector<std::string> conflicts;
          for (std::size_t k = 0; k < w; ++k) {
            for (auto& [key, val] : updates[k].items()) {
              (void)val;
              auto it = first_writer.find(key);
              if (it == first_writer.end()) {
                first_writer.emplace(key, k);
              } else if (!schema_.has_reducer(key)) {
                conflicts.push_back("'" + key + "' written by nodes '" +
                                    members[it->second].node + "' & '" + members[k].node + "'");
              }
            }
          }
          if (!conflicts.empty()) {
            std::string msg = "并行波写冲突（默认 overwrite 键被多个成员并发写）: ";
            for (std::size_t c = 0; c < conflicts.size(); ++c) {
              if (c) msg += "; ";
              msg += conflicts[c];
            }
            msg += " —— 请给并行步骤分配独立输出键或用累积类 reducer";
            finish();
            return std::unexpected(GraphError{GraphError::Kind::ConcurrentWriteConflict,
                                              std::move(msg)});
          }
        }

        // 按声明序归约（与串行逐包归约同型）+ 按序发射 node_end
        for (std::size_t k = 0; k < w; ++k) {
          state = schema_.apply(state, updates[k]);
          emit(StreamEvent{.type = "node_end", .node = members[k].node, .node_kind = "",
                           .step = static_cast<int>(base + k), .payload = updates[k], .ok = true,
                           .status = "ok"});
        }
        if ((mode_ & EventMode::Snapshot) != EventMode::Off)
          emit(StreamEvent{.type = "values", .node = "", .node_kind = "",
                           .step = static_cast<int>(base), .payload = state, .ok = true,
                           .status = "ok"});

        // 各成员产物按成员序追加入队（后续继续 FIFO / 再成波）
        for (std::size_t k = 0; k < w; ++k)
          for (auto& p : produced[k]) current.push_back(std::move(p));

        // 波不可分割：join 后落**一个** wave 快照（恢复语义 = 回到波起点边界继续）
        if (cp_ != nullptr && !thread_id.empty()) {
          Json meta = {{"step", base}, {"node", "wave"}};
          last_checkpoint_ = cp_->put(thread_id, "", parent, state, to_packets(current), meta);
          parent = last_checkpoint_;
        }
        last_steps_ += w;
        continue;
      }
    }

    state::Packet pkt = current.front();
    current.pop_front();
    std::string const node = std::move(pkt.node);
    if (node == kEnd) break;

    // problems/07 §8 P1：宿主取消（节点边界）。已执行节点的 update 已在 state/snapshot 里；
    // 之后不再跑任何节点，以 Cancelled 结束（由宿主决定落 _cancelled 档/退出码）。
    if (opts.should_stop && opts.should_stop()) {
      finish();
      return std::unexpected(GraphError{GraphError::Kind::Cancelled, "cancelled by host"});
    }

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
    ctx.shared_tools = opts_.shared_tools;  // wfr-㊹：workflow 共享工具注册表（tool_registry 注入）
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

  } catch (...) {
    finish();  // 异常兜底：非 std 节点异常 / schema.apply / 落盘失败 / 宿主 sink 抛 ——
               // 保证注入设施不残留（不吞异常，继续传播给调用方）
    throw;
  }
}

// 便捷收集：跑一遍并把事件全收下来（供测试/demo 观察；唯一执行通道仍是 invoke）
std::vector<StreamEvent> CompiledGraph::stream(Json const& input, std::string const& thread_id) {
  std::vector<StreamEvent> out;
  EventSink collector = [&out](StreamEvent const& e) { out.push_back(e); };
  (void)invoke(input, thread_id, InvokeOptions{collector, EventMode::All, std::nullopt});
  return out;
}

}  // namespace cppchain::graph