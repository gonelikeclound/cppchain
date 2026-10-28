# M1 消息层测试（GTest，FetchContent 拉取）
include(FetchContent)
include(GoogleTest)
set(INSTALL_GTEST OFF CACHE BOOL "不安装 googletest（发布只装 wfr 等正式产物）" FORCE)
FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest
  GIT_TAG v1.15.2)
FetchContent_MakeAvailable(googletest)

add_executable(message_tests
  message/message_serialization_test.cpp
  message/chunk_merge_test.cpp
  message/utf8_test.cpp
  message/phantasma_signal_test.cpp
  message/phantasma_hook_test.cpp)

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
  tool/tool_category_test.cpp
  tool/web_search_test.cpp
  tool/datetime_test.cpp
  tool/fetch_url_test.cpp
  tool/calculator_test.cpp
  tool/file_tools_test.cpp
  tool/exec_guard_test.cpp
  tool/command_classify_test.cpp
  tool/bash_tool_test.cpp
  tool/sandbox_test.cpp
  tool/arxiv_test.cpp
  tool/hybrid_selector_test.cpp
  tool/effect_class_test.cpp
  tool/mcp_client_test.cpp
  tool/mcp_discover_test.cpp
  tool/mcp_listen_test.cpp
  tool/image_test.cpp)

target_link_libraries(tool_tests PRIVATE
  cppchain_tool
  GTest::gtest_main
  Threads::Threads)

# so:// 发现路线的测试插件落点（见上方 mcp_list_plugin target）
target_compile_definitions(tool_tests PRIVATE
  MCP_TEST_SO_DIR="${CMAKE_CURRENT_BINARY_DIR}")

target_compile_options(tool_tests PRIVATE -Wall -Wextra -Wpedantic)
gtest_discover_tests(tool_tests)
# ---- M5: 状态层测试（state/reducer + checkpointer + store）----
add_executable(state_tests
  state/state_test.cpp
  state/checkpointer_test.cpp
  state/store_test.cpp
  state/messages_op_test.cpp)

target_link_libraries(state_tests PRIVATE
  cppchain_state
  GTest::gtest_main
  Threads::Threads)

target_compile_options(state_tests PRIVATE -Wall -Wextra -Wpedantic)
gtest_discover_tests(state_tests)

# ---- M6: Graph 引擎测试（含 M8 的 agent 适配器测试 → 需 graph_agent）----
add_executable(graph_tests
  graph/graph_test.cpp
  graph/agent_node_test.cpp
  graph/streaming_test.cpp
  graph/interrupt_test.cpp
  graph/send_test.cpp
  graph/send_parallel_test.cpp
  graph/time_travel_test.cpp)

target_link_libraries(graph_tests PRIVATE
  cppchain_graph_agent
  GTest::gtest_main
  Threads::Threads)

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
  agent/approval_memo_test.cpp
  agent/history_compress_test.cpp
  agent/history_rules_test.cpp
  agent/artifact_fs_test.cpp)

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

# wfr-㊹ 治理（problems/34）：动态 mock 工具库 —— 纯 C descriptor + 裸函数指针，不链 cppchain
add_library(mock_tools SHARED plugin_demo/mock_tools.cpp)
target_include_directories(mock_tools PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_include_directories(mock_tools SYSTEM BEFORE PRIVATE ${nlohmann_json_SOURCE_DIR}/include)
target_compile_options(mock_tools PRIVATE -Wall -Wextra -Wpedantic)

# MCP 二期：so:// 发现路线的测试插件（dlsym `mcp_list_<fn>` → 列表 JSON 字符串）。纯 C ABI，
# 不链 cppchain（同 mock_tools 纪律）。MCP_TEST_SO_DIR 注入 tool_tests，运行时 dlopen 用。
add_library(mcp_list_plugin SHARED plugin_mcp/mcp_list_plugin.cpp)
target_include_directories(mcp_list_plugin PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_include_directories(mcp_list_plugin SYSTEM BEFORE PRIVATE ${nlohmann_json_SOURCE_DIR}/include)
target_compile_options(mcp_list_plugin PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(mcp_list_plugin PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# wfr-㊹：副作用场景 mock 库（订单/微信可撤/短信不可撤/电话可关 + 模拟时钟），副作用测试专属
add_library(sidefx_notify SHARED plugin_demo/sidefx_notify.cpp)
target_include_directories(sidefx_notify PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_include_directories(sidefx_notify SYSTEM BEFORE PRIVATE ${nlohmann_json_SOURCE_DIR}/include)
target_compile_options(sidefx_notify PRIVATE -Wall -Wextra -Wpedantic)

# problems/07 §9.4：取消端到端用例的"慢工具"库（slow_sleep，只读）；wfr 经 WFR_TOOL_LIB_DIRS 找到
add_library(slow_tools SHARED plugin_demo/slow_tools.cpp)
target_include_directories(slow_tools PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_include_directories(slow_tools SYSTEM BEFORE PRIVATE ${nlohmann_json_SOURCE_DIR}/include)
target_compile_options(slow_tools PRIVATE -Wall -Wextra -Wpedantic)

add_executable(workflow_tests
  wf/expr_test.cpp
  wf/context_node_test.cpp
  wf/refs_test.cpp
  wf/config_decl_test.cpp
  wf/ir_validator_test.cpp
  wf/compiler_test.cpp
  wf/generator_test.cpp
  wf/features_test.cpp
  wf/richtools_test.cpp
  wf/fakes_test.cpp
  wf/agent_loop_test.cpp
  wf/side_effect_workflow_test.cpp
  wf/tool_registry_select_test.cpp
  wf/sidefx_notify_test.cpp
  wf/tool_loop_test.cpp
  wf/tool_result_artifact_test.cpp
  wf/file_context_test.cpp
  wf/session_lock_test.cpp
  wf/fire_cancel_test.cpp
  wf/wfr_cancel_cli_test.cpp
  wf/wfr_cgi_cancel_test.cpp
  wf/cancel_e2e_test.cpp
  wf/session_channel_test.cpp
  wf/approve_channel_test.cpp
  wf/session_state_test.cpp
  wf/phantasma_wiring_test.cpp
  wf/verify_agent_test.cpp
  wf/artifact_ver_test.cpp
  wf/approve_tools_test.cpp
  wf/approve_tools_interrupt_test.cpp
  wf/approve_tools_persist_test.cpp
  wf/plan_execute_l1_test.cpp
  wf/plan_execute_l2_test.cpp
  wf/plan_execute_l3_test.cpp
  wf/plan_review_test.cpp
  wf/mch_test.cpp
  wf/parallel_mch_test.cpp
  wf/proxy_node_test.cpp
  wf/project_test.cpp
  wf/project_review_test.cpp
  wf/project_drive_test.cpp
  wf/governance_test.cpp
  wf/lot_task_test.cpp
  wf/history_nodes_test.cpp
  wf/plugin_import_test.cpp
  wf/m10_runtime_test.cpp
  wf/import_workflow_test.cpp
  wf/echo_test.cpp
  wf/debug_view_test.cpp
  wf/model_params_test.cpp
  wf/loop_test.cpp
  wf/history_ops_test.cpp
  wf/history_process_test.cpp
  wf/wfr_lookup_test.cpp
  wf/cron_batch_test.cpp
  wf/longdoc_test.cpp)

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
  CPPCHAIN_NODE_ENUM_SO="$<TARGET_FILE:node_enum_plugin>"
  # wfr-㊹：mock_tools 动态工具库的**目录**（tool_registry 按 `lib<from>.so` 在此找库）
  CPPCHAIN_MOCK_TOOLS_DIR="$<TARGET_FILE_DIR:mock_tools>"
  # 副作用场景 mock 库目录（sidefx_notify_test.cpp 用它定位 + state_reset 隔离）
  CPPCHAIN_SIDEFX_NOTIFY_DIR="$<TARGET_FILE_DIR:sidefx_notify>")
add_dependencies(workflow_tests node_demo_plugin node_enum_plugin mock_tools sidefx_notify slow_tools wfr)

target_compile_options(workflow_tests PRIVATE -Wall -Wextra -Wpedantic)
gtest_discover_tests(workflow_tests)

# wfr-㊹：dev 构建让 wfr 真机能自动找到副作用 mock 库（tool_lib_dirs 编译宏兜底）。
# 只在此 (tests ON) 环境下联动；release（tests OFF）靠 WFR_TOOL_LIB_DIRS env / 安装到 <prefix>/lib。
if(TARGET wfr)
  add_dependencies(wfr sidefx_notify)
  target_compile_definitions(wfr PRIVATE
    WFR_TOOL_LIB_DIRS="$<TARGET_FILE_DIR:sidefx_notify>")
  install(TARGETS sidefx_notify LIBRARY DESTINATION lib)
endif()

# ---- 调试台页面 JS 的推导测试（wfr-㉟）----
# 日志的推导（分组 / loop 迭代号 / 走过的边）在**页面里的 JS**（唯一实现，因为"拖入日志
# 文件"必须在浏览器里推导）。这条测试把模板抠出来真跑一遍；没装 node 就不注册。
find_program(NODE_EXECUTABLE node)
if(NODE_EXECUTABLE)
  add_test(NAME debug_derive_js
           COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/js/debug_derive_test.mjs
                   ${PROJECT_SOURCE_DIR})
  message(STATUS "已注册 debug_derive_js（页面 JS 推导测试，node: ${NODE_EXECUTABLE}）")
else()
  message(STATUS "未找到 node —— 跳过 debug_derive_js（页面 JS 的推导测试不跑）")
endif()

# ---- CGI WebUI 纯逻辑测试（wfr-🈟 增强）：header-only cgi_lib，无需链 CGI 可执行 ----
add_executable(webui_tests
  webui/cgi_lib_test.cpp)

target_include_directories(webui_tests PRIVATE ${PROJECT_SOURCE_DIR}/cgi-bin)
target_link_libraries(webui_tests PRIVATE
  cppchain_message
  GTest::gtest_main)

target_compile_options(webui_tests PRIVATE -Wall -Wextra -Wpedantic)
gtest_discover_tests(webui_tests)
