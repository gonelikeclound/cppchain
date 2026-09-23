# examples/ —— M1 用法 demo 与后续问题验证载体
# 每个 demo 一个可执行目标；support/ 为 demo 共享的展示助手。
# 索引与 langchain 参照见 examples/README.md
set(EXAMPLE_TARGETS
  messages_basic          # 消息类型族
  messages_serialize      # JSON round-trip + 落盘/回读
  messages_tool_flow      # 工具调用消息协议闭环 + 不变式
  messages_stream_chunk   # 流式 chunk 合并 → 收敛
)
foreach(t IN LISTS EXAMPLE_TARGETS)
  add_executable(${t} ${t}/main.cpp)
  target_link_libraries(${t} PRIVATE cppchain_message)
  target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_options(${t} PRIVATE -Wall -Wextra -Wpedantic)
endforeach()