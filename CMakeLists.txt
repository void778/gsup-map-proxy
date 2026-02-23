cmake_minimum_required(VERSION 3.16)
project(gsup_map_proxy VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

option(BUILD_TESTS            "Build unit tests"                ON)
option(ENABLE_COVERAGE        "Enable code coverage"            OFF)
option(ENABLE_SCTP_TRANSPORT  "Enable SCTP socket for M3UA"     OFF)

add_compile_options(-Wall -Wextra -Wpedantic)

if(ENABLE_COVERAGE)
  add_compile_options(--coverage -O0)
  add_link_options(--coverage)
endif()

find_package(Threads REQUIRED)
# Boost.Asio is header-only since Boost 1.74; no compiled component needed.
find_package(Boost 1.71 REQUIRED)

# ── spdlog (structured logging) ───────────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(
  spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG        v1.13.0
  GIT_SHALLOW    TRUE
  GIT_SUBMODULES ""
)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(spdlog)
# Provide a header-only interface target for transport_lib.
if(NOT TARGET Boost::headers)
  add_library(Boost::headers INTERFACE IMPORTED)
  set_target_properties(Boost::headers PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${Boost_INCLUDE_DIRS}")
endif()

if(BUILD_TESTS)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  enable_testing()
  include(GoogleTest)
endif()

add_subdirectory(src)

if(BUILD_TESTS)
  add_subdirectory(tests)
endif()
