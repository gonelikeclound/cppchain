# examples/ —— 每个 demo 一个可执行目标，demo 与 04-problems 的问题验证一一对应。
add_executable(message_pipeline message_pipeline/main.cpp)
target_link_libraries(message_pipeline PRIVATE cppchain_message)
target_compile_options(message_pipeline PRIVATE -Wall -Wextra -Wpedantic)