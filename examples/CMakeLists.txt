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
set(TOOL_EXAMPLE_TARGETS tools_basic tools_retrieval tools_retrieval_vector)
# 需要状态库的 demo
set(STATE_EXAMPLE_TARGETS state_basics)
# 需要 graph 库的 demo
set(GRAPH_EXAMPLE_TARGETS graph_basics)
# 需要 agent 库（真机）的 demo
set(AGENT_EXAMPLE_TARGETS agent_loop context_budget)
set(HTTP_NEEDED_EXAMPLES tools_retrieval_vector structured_output agent_loop context_budget)  # 需要真网络（embedding 端点）

list(APPEND EXAMPLE_TARGETS tools_basic tools_retrieval state_basics graph_basics)
if(CPPCHAIN_BUILD_HTTP)
  list(APPEND EXAMPLE_TARGETS tools_retrieval_vector)
  list(APPEND EXAMPLE_TARGETS agent_loop)
  list(APPEND EXAMPLE_TARGETS context_budget)
endif()

if(CPPCHAIN_BUILD_HTTP)
  list(APPEND EXAMPLE_TARGETS chat_live)
  list(APPEND EXAMPLE_TARGETS structured_output)
endif()

foreach(t IN LISTS EXAMPLE_TARGETS)
  add_executable(${t} ${t}/main.cpp)
  target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_options(${t} PRIVATE -Wall -Wextra -Wpedantic)
  if(t IN_LIST AGENT_EXAMPLE_TARGETS)
    target_link_libraries(${t} PRIVATE cppchain_agent cppchain_http_curl)
  elseif(t IN_LIST HTTP_NEEDED_EXAMPLES)
    target_link_libraries(${t} PRIVATE cppchain_tool cppchain_http_curl)
  elseif(t IN_LIST TOOL_EXAMPLE_TARGETS)
    target_link_libraries(${t} PRIVATE cppchain_tool)
  elseif(t IN_LIST GRAPH_EXAMPLE_TARGETS)
    target_link_libraries(${t} PRIVATE cppchain_graph)
  elseif(t IN_LIST STATE_EXAMPLE_TARGETS)
    target_link_libraries(${t} PRIVATE cppchain_state)
  elseif(t IN_LIST HTTP_EXAMPLE_TARGETS)
    target_link_libraries(${t} PRIVATE cppchain_http_curl)
  elseif(t IN_LIST MODEL_EXAMPLE_TARGETS)
    target_link_libraries(${t} PRIVATE cppchain_model)
  else()
    target_link_libraries(${t} PRIVATE cppchain_message)
  endif()
endforeach()