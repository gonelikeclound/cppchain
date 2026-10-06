# examples/ —— M1 用法 demo 与后续问题验证载体
# 每个 demo 一个可执行目标；support/ 为 demo 共享的展示助手。
# 索引与 langchain 参照见 examples/README.md
set(EXAMPLE_TARGETS
  messages_basic          # 消息类型族
  messages_serialize      # JSON round-trip + 落盘/回读
  messages_tool_flow      # 工具调用消息协议闭环 + 不变式
  messages_stream_chunk   # 流式 chunk 合并 → 收敛
  chat_invoke             # M2: ChatModel 同步/协程调用（离线仿真）
  chat_stream             # M2: 流式增量 + 回调 + 取消（离线仿真）
)

# 需要模型库的 demo
set(MODEL_EXAMPLE_TARGETS chat_invoke chat_stream structured_output)
# 需要真实 HTTP 传输的 demo（CPPCHAIN_BUILD_HTTP=ON 时才存在）
set(HTTP_EXAMPLE_TARGETS chat_live)
# 需要工具库的 demo
set(TOOL_EXAMPLE_TARGETS tools_basic tools_retrieval tools_retrieval_vector
                         tools_frontline tools_files tools_arxiv m9_hybrid)
# 需要状态库的 demo
set(STATE_EXAMPLE_TARGETS state_basics)
# 需要 graph 库的 demo
set(GRAPH_EXAMPLE_TARGETS graph_basics m10_streaming m10_interrupt m10_send m10_fork)
# 主动智能 workflow 生成端到端验收（真机 LLM）+ wfr 命令行运行器
set(WF_EXAMPLE_TARGETS wf_eval proactive workflow_cli wfr)
# 需要 agent 库（真机）的 demo
set(AGENT_EXAMPLE_TARGETS agent_loop context_budget stale_state live_tools history_agent research_chat agent_cli session_resume)
# M8 子代理 demo：额外依赖 graph 库（as_agent_node / StateGraph）
set(SUBAGENT_EXAMPLE_TARGETS subagent_ask_expert subagent_workers subagent_handoff)
list(APPEND AGENT_EXAMPLE_TARGETS ${SUBAGENT_EXAMPLE_TARGETS})
# M9 真机量化 demo
list(APPEND AGENT_EXAMPLE_TARGETS m9_budget)
# 离线（用 FakeTransport/无网络）但需要 agent 库的 demo
set(OFFLINE_AGENT_EXAMPLES side_effect_guard tools_approval m9_approval m9_artifact)
set(HTTP_NEEDED_EXAMPLES tools_retrieval_vector structured_output agent_loop context_budget stale_state)  # 需要真网络（embedding 端点）

list(APPEND EXAMPLE_TARGETS tools_basic tools_retrieval state_basics graph_basics side_effect_guard
                            tools_frontline tools_files tools_arxiv tools_approval
                            m9_hybrid m9_approval m9_artifact
                            m10_streaming m10_interrupt m10_send m10_fork)
if(CPPCHAIN_BUILD_HTTP)
  list(APPEND EXAMPLE_TARGETS tools_retrieval_vector)
  list(APPEND EXAMPLE_TARGETS agent_loop)
  list(APPEND EXAMPLE_TARGETS context_budget)
  list(APPEND EXAMPLE_TARGETS stale_state)
  list(APPEND EXAMPLE_TARGETS live_tools)
  list(APPEND EXAMPLE_TARGETS history_agent)
  list(APPEND EXAMPLE_TARGETS session_resume)
  list(APPEND EXAMPLE_TARGETS research_chat)
  list(APPEND EXAMPLE_TARGETS agent_cli)
  list(APPEND EXAMPLE_TARGETS subagent_ask_expert)
  list(APPEND EXAMPLE_TARGETS subagent_workers)
  list(APPEND EXAMPLE_TARGETS subagent_handoff)
  list(APPEND EXAMPLE_TARGETS m9_budget)
endif()

if(CPPCHAIN_BUILD_HTTP)
  list(APPEND EXAMPLE_TARGETS chat_live)
  list(APPEND EXAMPLE_TARGETS structured_output)
  list(APPEND EXAMPLE_TARGETS ${WF_EXAMPLE_TARGETS})
endif()

foreach(t IN LISTS EXAMPLE_TARGETS)
  add_executable(${t} ${t}/main.cpp)
  target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_options(${t} PRIVATE -Wall -Wextra -Wpedantic)
  if(t IN_LIST AGENT_EXAMPLE_TARGETS)
    if(t IN_LIST SUBAGENT_EXAMPLE_TARGETS)
      # 用到 as_agent_node → 需要 graph_agent 适配器库（它 PUBLIC 带出 graph 与 agent）
      target_link_libraries(${t} PRIVATE cppchain_graph_agent cppchain_agent cppchain_http_curl)
    else()
      target_link_libraries(${t} PRIVATE cppchain_agent cppchain_http_curl)
    endif()
  elseif(t IN_LIST OFFLINE_AGENT_EXAMPLES)
    target_link_libraries(${t} PRIVATE cppchain_agent)
  elseif(t IN_LIST HTTP_NEEDED_EXAMPLES)
    target_link_libraries(${t} PRIVATE cppchain_tool cppchain_http_curl)
  elseif(t IN_LIST TOOL_EXAMPLE_TARGETS)
    target_link_libraries(${t} PRIVATE cppchain_tool)
  elseif(t IN_LIST GRAPH_EXAMPLE_TARGETS)
    target_link_libraries(${t} PRIVATE cppchain_graph)
  elseif(t IN_LIST STATE_EXAMPLE_TARGETS)
    target_link_libraries(${t} PRIVATE cppchain_state)
  elseif(t IN_LIST WF_EXAMPLE_TARGETS)
    target_link_libraries(${t} PRIVATE cppchain_workflow cppchain_http_curl)
  elseif(t IN_LIST HTTP_EXAMPLE_TARGETS)
    # chat_live 真机联调：既用传输层也用模型层。两库都显式列出
    # （此前靠 http_curl 的传递依赖白蹭 model，已随依赖修正一并去掉）。
    target_link_libraries(${t} PRIVATE cppchain_http_curl cppchain_model)
  elseif(t IN_LIST MODEL_EXAMPLE_TARGETS)
    target_link_libraries(${t} PRIVATE cppchain_model)
  else()
    target_link_libraries(${t} PRIVATE cppchain_message)
  endif()
endforeach()