# 源码入口。自底向上逐步加入：message → model → tool → state → graph → agent ...
# 每个模块一个 static lib，demo 与问题验证在顶层 examples/。

# ---- M1: cppchain_message ----
add_library(cppchain_message STATIC
  message/message.cpp)

target_include_directories(cppchain_message PUBLIC
  ${PROJECT_SOURCE_DIR}/include)

# nlohmann 头文件在 C++26 下会告警(std::is_trivial deprecated)——来自第三方,
# 用 SYSTEM include 压制,避免污染我们自己的零告警目标。
target_include_directories(cppchain_message SYSTEM BEFORE PUBLIC
  ${nlohmann_json_SOURCE_DIR}/include)

target_link_libraries(cppchain_message PUBLIC
  nlohmann_json::nlohmann_json)

target_compile_options(cppchain_message PRIVATE -Wall -Wextra -Wpedantic)

# ---- M2: cppchain_model（ChatModel 抽象 + OpenAI 兼容适配）----
# 不链接 libcurl：传输通过 http::Transport 注入，保持分层干净。
add_library(cppchain_model STATIC
  model/openai_chat_model.cpp)

target_include_directories(cppchain_model PUBLIC
  ${PROJECT_SOURCE_DIR}/include)

target_include_directories(cppchain_model SYSTEM BEFORE PUBLIC
  ${nlohmann_json_SOURCE_DIR}/include)

target_link_libraries(cppchain_model PUBLIC
  cppchain_message
  nlohmann_json::nlohmann_json)

target_compile_options(cppchain_model PRIVATE -Wall -Wextra -Wpedantic)

# ---- 工具链冒烟（随首模块落地后会替换/保留） ----
add_executable(smoke smoke/main.cpp)
target_link_libraries(smoke PRIVATE cppchain_message)
target_compile_options(smoke PRIVATE -Wall -Wextra -Wpedantic)