cmake_minimum_required(VERSION 3.28)
project(super_feiyu LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# 本地依赖前缀（无 root 环境用 apt-get download + dpkg -x 解包到 ~/local）
set(FISH_LOCAL_PREFIX "$ENV{HOME}/local" CACHE PATH "Local dependency prefix")

find_path(CURL_INCLUDE_DIR curl/curl.h
  PATHS ${FISH_LOCAL_PREFIX}/include/x86_64-linux-gnu ${FISH_LOCAL_PREFIX}/include
        /usr/include/x86_64-linux-gnu /usr/include)
find_library(CURL_LIBRARY NAMES curl
  PATHS ${FISH_LOCAL_PREFIX}/lib /usr/lib/x86_64-linux-gnu)
find_path(SQLITE_INCLUDE_DIR sqlite3.h
  PATHS ${FISH_LOCAL_PREFIX}/include /usr/include)
find_library(SQLITE_LIBRARY NAMES sqlite3
  PATHS ${FISH_LOCAL_PREFIX}/lib /usr/lib/x86_64-linux-gnu)

if(NOT CURL_INCLUDE_DIR OR NOT SQLITE_INCLUDE_DIR)
  message(FATAL_ERROR "missing curl/sqlite3 headers; set FISH_LOCAL_PREFIX")
endif()

# 模块搜索路径：本项目源码 + 第三方头（nlohmann 等）
set(FISH_MODULE_INCLUDE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${FISH_LOCAL_PREFIX}/include
  ${FISH_LOCAL_PREFIX}/include/x86_64-linux-gnu
)

# 所有模块作为同一组文件集，由 CMake 负责编译顺序与依赖分析
add_library(fish_modules OBJECT
  src/fish.core.cppm
  src/fish.log.cppm
  src/fish.config.cppm
  src/fish.memory.cppm
  src/fish.capability.cppm
  src/fish.transport.cppm
  src/fish.orchestrator.cppm
)
target_include_directories(fish_modules PUBLIC ${FISH_MODULE_INCLUDE})
target_link_libraries(fish_modules PUBLIC ${CURL_LIBRARY} ${SQLITE_LIBRARY})
target_compile_options(fish_modules PRIVATE
  $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall -Wextra>
)

add_executable(super-feiyu src/main.cpp)
target_link_libraries(super-feiyu PRIVATE fish_modules)

enable_testing()

add_executable(test_all tests/test_all.cpp)
target_link_libraries(test_all PRIVATE fish_modules)
add_test(NAME all COMMAND test_all)
