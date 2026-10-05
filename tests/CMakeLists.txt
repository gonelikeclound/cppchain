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

# ---- M6: Graph 引擎测试（含 M8 的 agent 适配器测试 → 需 graph_agent）----
add_executable(graph_tests
  graph/graph_test.cpp
  graph/agent_node_test.cpp)

target_link_libraries(graph_tests PRIVATE
  cppchain_graph_agent
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

# ---- 主动智能 workflow：IR/引用/表达式/校验/编译/生成/特征/动态库 ----
# （feaked 工具离线确定性；生成器用 ScriptedChatModel 全离线驱动）
# 特征谓词 + 12 例固件在 examples/support/features.hpp（仅 demo/评测用，不进库）：
# 这里是纯 header，加 examples/ 头路径直接引用同一份。
add_library(node_demo_plugin SHARED plugin_demo/node_demo_plugin.cpp)
target_include_directories(node_demo_plugin PRIVATE ${PROJECT_SOURCE_DIR}/include)
# nlohmann 纯头：SYSTEM include 压制其第三方告警（与其它 target 同一套做法）——
# .so 不需要链接任何 cppchain 静态库（静态库非 -fPIC 塞进 .so 会 ld 报错；
# 插件对 cc.graph.* / message::* 的引用保持未定义，dlopen 时宿主 --export-dynamic 解析）
target_include_directories(node_demo_plugin SYSTEM BEFORE PRIVATE ${nlohmann_json_SOURCE_DIR}/include)
target_compile_options(node_demo_plugin PRIVATE -Wall -Wextra -Wpedantic)

# ⑤b（A1）E2E 用的"全好"库：省略 nodes 走枚举时它能整库干净加载
add_library(node_enum_plugin SHARED plugin_demo/node_enum_plugin.cpp)
target_include_directories(node_enum_plugin PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_include_directories(node_enum_plugin SYSTEM BEFORE PRIVATE ${nlohmann_json_SOURCE_DIR}/include)
target_compile_options(node_enum_plugin PRIVATE -Wall -Wextra -Wpedantic)

add_executable(workflow_tests
  wf/expr_test.cpp
  wf/refs_test.cpp
  wf/ir_validator_test.cpp
  wf/compiler_test.cpp
  wf/generator_test.cpp
  wf/features_test.cpp
  wf/richtools_test.cpp
  wf/fakes_test.cpp
  wf/agent_loop_test.cpp
  wf/history_nodes_test.cpp
  wf/plugin_import_test.cpp)

target_include_directories(workflow_tests PRIVATE ${PROJECT_SOURCE_DIR}/examples)

target_link_libraries(workflow_tests PRIVATE
  cppchain_workflow
  GTest::gtest_main
  Threads::Threads
  ${CMAKE_DL_LIBS})

# 把宿主（workflow_tests）的符号导出进全局作用域，供 dlopen 的插件 .so 解析：
# （StateGraph::* / message::* 等在 .so 里保持未定义，加载时从这里找）
target_link_options(workflow_tests PRIVATE -Wl,--export-dynamic)

# 给测试 bake 进 .so 的绝对路径（dlopen/枚举用）；依赖关系保证先构建
target_compile_definitions(workflow_tests PRIVATE
  CPPCHAIN_NODE_DEMO_SO="$<TARGET_FILE:node_demo_plugin>"
  CPPCHAIN_NODE_ENUM_SO="$<TARGET_FILE:node_enum_plugin>")
add_dependencies(workflow_tests node_demo_plugin node_enum_plugin)

target_compile_options(workflow_tests PRIVATE -Wall -Wextra -Wpedantic)
gtest_discover_tests(workflow_tests)
