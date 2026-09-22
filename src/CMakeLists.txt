# 顶层源码入口。后续自底向上加入：
#   message / model / prompt_template / output_parser / tool / agent / graph ...
# 每个模块一个子目标（static lib），demo 与问题验证放在顶层 examples/。

add_executable(smoke smoke/main.cpp)
target_compile_options(smoke PRIVATE -Wall -Wextra -Wpedantic)