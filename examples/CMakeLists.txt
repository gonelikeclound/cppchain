# examples/ —— M1 用法 demo 与后续问题验证载体
# 每个 demo 一个可执行目标；support/ 为 demo 共享的展示助手。
# 索引与 langchain 参照见 examples/README.md
find_package(Threads REQUIRED)  # 并行波 demo（m10_send_parallel）用 std::thread
set(EXAMPLE_TARGETS
  messages_basic          # 消息类型族
  messages_serialize      # JSON round-trip + 落盘/回读
  messages_tool_flow      # 工具调用消息协议闭环 + 不变式
  messages_stream_chunk   # 流式 chunk 合并 → 收敛
  chat_invoke             # M2: ChatModel 同步/协程调用（离线仿真）
  chat_stream             # M2: 流式增量 + 回调 + 取消（离线仿真）
  vision_basic            # 多模态：ImageBlock → image_url，离线识别演示
)

# 需要模型库的 demo
set(MODEL_EXAMPLE_TARGETS chat_invoke chat_stream structured_output vision_basic)
# 需要真实 HTTP 传输的 demo（CPPCHAIN_BUILD_HTTP=ON 时才存在）
set(HTTP_EXAMPLE_TARGETS chat_live)
# 需要工具库的 demo
set(TOOL_EXAMPLE_TARGETS tools_basic tools_retrieval tools_retrieval_vector
                         tools_frontline tools_files tools_arxiv m9_hybrid
                         bash_sidefx_analysis
                         mcp_provider      # MCP 二期：三路发现 + provider 节点（离线）
                         vision_agent)     # 看图→调工具→回答 组合 demo（离线）
# 需要状态库的 demo
set(STATE_EXAMPLE_TARGETS state_basics)
# MCP 全链路验证对：mock server（自包含 HTTP MCP，零凭证）+ driver（真 curl 传输层全链断言）
set(MCP_MOCK_TARGETS mcp_mock mcp_mock_driver)

# MCP 二期离线 demo 的 so:// 发现插件（dlsym `mcp_list_<fn>` → 列表 JSON，纯 C ABI）：
# 与 tests/plugin_mcp 同纪律，不链 cppchain 静态库。产物落 examples 构建目录供 demo dlopen。
add_library(mcp_weather_list SHARED mcp_provider/list_plugin.cpp)
target_include_directories(mcp_weather_list PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_include_directories(mcp_weather_list SYSTEM BEFORE PRIVATE ${nlohmann_json_SOURCE_DIR}/include)
target_compile_options(mcp_weather_list PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(mcp_weather_list PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
# 需要 graph 库的 demo
set(GRAPH_EXAMPLE_TARGETS graph_basics m10_streaming m10_interrupt m10_send m10_send_parallel m10_fork)
# wfr-㊸ @②：workflow 级人工审批 HITL（interrupt 悬挂/恢复）demo —— 只需 workflow 本体
set(HITL_EXAMPLE_TARGETS approve_interrupt)
# wfr-㊿ proxyNode（arch 24）demo：运行期调用子 workflow —— 只需 workflow 本体（纯离线）
set(PROXY_EXAMPLE_TARGETS proxy_call)
# P1.5 声明式配置（arch 25）demo：declarations.configs 三形态 + ${workdir} 运行变量（纯离线）
set(CONFIG_DECL_EXAMPLE_TARGETS config_decl)
# 主动智能 workflow 生成端到端验收（真机 LLM）+ wfr 已在 src/wfr（正式 CLI，非 examples）
set(WF_EXAMPLE_TARGETS wf_eval proactive workflow_cli)
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
                            m9_hybrid m9_approval m9_artifact bash_sidefx_analysis
                            mcp_provider  # MCP 二期离线 demo（三路发现 + provider）
                            mcp_mock      # 自包含 mock MCP server（HTTP/JSON-RPC + SSE 推送）
                            vision_agent   # 多模态 agent 组合（看图→工具→回答）
                            m10_streaming m10_interrupt m10_send m10_send_parallel m10_fork)
list(APPEND EXAMPLE_TARGETS approve_interrupt)
list(APPEND EXAMPLE_TARGETS ${PROXY_EXAMPLE_TARGETS})  # proxy_call：wfr-㊿ 子 workflow 调用（离线）
list(APPEND EXAMPLE_TARGETS ${CONFIG_DECL_EXAMPLE_TARGETS})  # config_decl：P1.5 声明式配置（离线）
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
  # MCP 全链路验证 driver：需要真实传输层（真 curl 连 mock server）
  list(APPEND EXAMPLE_TARGETS mcp_mock_driver)
  list(APPEND HTTP_NEEDED_EXAMPLES mcp_mock_driver)
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
    target_link_libraries(${t} PRIVATE cppchain_graph Threads::Threads)
  elseif(t IN_LIST STATE_EXAMPLE_TARGETS)
    target_link_libraries(${t} PRIVATE cppchain_state)
  elseif(t IN_LIST HITL_EXAMPLE_TARGETS)
    target_link_libraries(${t} PRIVATE cppchain_workflow)
  elseif(t IN_LIST PROXY_EXAMPLE_TARGETS)
    target_link_libraries(${t} PRIVATE cppchain_workflow Threads::Threads)
  elseif(t IN_LIST CONFIG_DECL_EXAMPLE_TARGETS)
    target_link_libraries(${t} PRIVATE cppchain_workflow)
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

# mcp_mock（纯 server）：零 cppchain 库依赖，只用 nlohmann（SYSTEM 压三方告警）
target_include_directories(mcp_mock SYSTEM BEFORE PRIVATE ${nlohmann_json_SOURCE_DIR}/include)

# mcp_provider demo：注入 so:// 发现插件目录（编译期绝对路径，同测试的 MCP_TEST_SO_DIR 做法）
target_compile_definitions(mcp_provider PRIVATE
  MCP_DEMO_SO_DIR="${CMAKE_CURRENT_BINARY_DIR}")

# ---- CGI WebUI（wfr_cgi）已移入 src/CMakeLists.txt（发布构建同 wfr 规格）----
