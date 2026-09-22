cmake_minimum_required(VERSION 3.28)

project(cppchain
  VERSION 0.1.0
  DESCRIPTION "C++26 简化复刻 langchain / langgraph —— LLM 与 Agent 开发学习项目"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# 语言标准：cppchain 明确定义为 C++26。
# CMake 3.28 不认识 cxx_std_26 meta-feature（会静默降级），
# 因此在 preset 中通过 CMAKE_CXX_FLAGS=-std=c++26 显式传入，并在此做护栏。
# ---------------------------------------------------------------------------
if(NOT CMAKE_CXX_FLAGS MATCHES "-std=(c\\+\\+26|gnu\\+\\+26)")
  message(FATAL_ERROR
    "cppchain 要求 C++26。请用 dev preset 配置（CMakePresets.json），"
    "或手动传 -DCMAKE_CXX_FLAGS=-std=c++26")
endif()

# 选项
option(CPPCHAIN_BUILD_HTTP       "是否启用 HTTP 传输层（依赖 libcurl）" OFF)
option(CPPCHAIN_BUILD_TESTS      "是否构建测试（FetchContent 拉取 GTest）" OFF)
option(CPPCHAIN_BUILD_EXAMPLES   "是否构建 examples/（demo 与问题验证）" ON)

# 基础依赖：nlohmann/json（header-only，M1 序列化契约就用它）
include(FetchContent)
set(CPPCHAIN_NLOHMANN_TAG v3.11.3)
FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json
  GIT_TAG ${CPPCHAIN_NLOHMANN_TAG})
FetchContent_MakeAvailable(nlohmann_json)

if(CPPCHAIN_BUILD_HTTP)
  find_package(CURL REQUIRED)
  message(STATUS "HTTP: libcurl ${CURL_VERSION_STRING}")
endif()

add_subdirectory(src)

if(CPPCHAIN_BUILD_TESTS)
  include(CTest)
  enable_testing()
  add_subdirectory(tests)
endif()

if(CPPCHAIN_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

message(STATUS "cppchain ${PROJECT_VERSION} | C++26 via ${CMAKE_CXX_COMPILER}")