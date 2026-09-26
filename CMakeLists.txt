cmake_minimum_required(VERSION 3.20)
project(CodeIcarus LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_subdirectory(core)
add_subdirectory(tools/cli)

enable_testing()
add_subdirectory(tests)
