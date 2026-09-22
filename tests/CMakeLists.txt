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