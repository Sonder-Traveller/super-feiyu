cmake_minimum_required(VERSION 3.20)
project(dafeiyu_core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# target 级 include，不用全局 include_directories
add_library(fish_core STATIC
  src/pipeline.cpp
  src/layered_persona.cpp
  src/reply_budget.cpp
  src/memory_store.cpp
)

target_include_directories(fish_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(fish_core PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
  $<$<CXX_COMPILER_ID:GNU,Clang>:$<$<CONFIG:Release>:-O3>>
  $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)

enable_testing()

add_executable(test_policy tests/test_policy.cpp)
target_link_libraries(test_policy PRIVATE fish_core)
add_test(NAME policy COMMAND test_policy)
