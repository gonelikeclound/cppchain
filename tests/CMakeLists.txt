# M1 消息层测试（GTest，FetchContent 拉取）
include(FetchContent)
include(GoogleTest)
FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest
  GIT_TAG v1.15.2)
FetchContent_MakeAvailable(googletest)

add_executable(message_tests
  message/message_serialization_test.cpp
  message/chunk_merge_test.cpp)

target_link_libraries(message_tests PRIVATE
  cppchain_message
  GTest::gtest_main)

target_compile_options(message_tests PRIVATE -Wall -Wextra -Wpedantic)
gtest_discover_tests(message_tests)

# ---- M2: 模型层测试（SSE / wire / ChatModel 行为）----
add_executable(model_tests
  model/openai_wire_test.cpp
  model/openai_model_test.cpp
  model/dashscope_embeddings_test.cpp
  model/structured_test.cpp)

target_link_libraries(model_tests PRIVATE
  cppchain_model
  GTest::gtest_main)

target_compile_options(model_tests PRIVATE -Wall -Wextra -Wpedantic)
gtest_discover_tests(model_tests)

# ---- M3: 工具层测试（规格 / 检索 / ToolNode）----
find_package(Threads REQUIRED)

add_executable(tool_tests
  tool/tool_spec_test.cpp
  tool/selector_test.cpp
  tool/tool_node_test.cpp
  tool/embedding_selector_test.cpp
  tool/weather_test.cpp
  tool/web_search_test.cpp
  tool/datetime_test.cpp
  tool/fetch_url_test.cpp
  tool/calculator_test.cpp
  tool/file_tools_test.cpp
  tool/arxiv_test.cpp
  tool/hybrid_selector_test.cpp)

target_link_libraries(tool_tests PRIVATE
  cppchain_tool
  GTest::gtest_main
  Threads::Threads)

target_compile_options(tool_tests PRIVATE -Wall -Wextra -Wpedantic)
gtest_discover_tests(tool_tests)
# ---- M5: 状态层测试（state/reducer + checkpointer + store）----
add_executable(state_tests
  state/state_test.cpp
  state/checkpointer_test.cpp
  state/store_test.cpp)

target_link_libraries(state_tests PRIVATE
  cppchain_state
  GTest::gtest_main
  Threads::Threads)

target_compile_options(state_tests PRIVATE -Wall -Wextra -Wpedantic)
gtest_discover_tests(state_tests)

# ---- M6: Graph 引擎测试 ----
add_executable(graph_tests
  graph/graph_test.cpp
  graph/agent_node_test.cpp)

target_link_libraries(graph_tests PRIVATE
  cppchain_graph
  GTest::gtest_main)

target_compile_options(graph_tests PRIVATE -Wall -Wextra -Wpedantic)
gtest_discover_tests(graph_tests)

# ---- M7/M9: Agent 循环与中间件测试 ----
add_executable(agent_tests
  agent/agent_test.cpp
  agent/subagent_test.cpp
  agent/journal_test.cpp
  agent/journal_persistence_test.cpp
  agent/stale_test.cpp
  agent/compression_freshness_test.cpp
  agent/tokens_test.cpp
  agent/trim_token_test.cpp
  agent/truncate_artifact_test.cpp
  agent/approval_memo_test.cpp)

target_link_libraries(agent_tests PRIVATE
  cppchain_agent
  GTest::gtest_main
  Threads::Threads)

target_compile_options(agent_tests PRIVATE -Wall -Wextra -Wpedantic)
gtest_discover_tests(agent_tests)
