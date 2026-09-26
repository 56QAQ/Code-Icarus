cmake_minimum_required(VERSION 3.20)

file(GLOB_RECURSE ICARUS_CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

add_library(icarus_core STATIC ${ICARUS_CORE_SOURCES})
target_include_directories(icarus_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(icarus_core PUBLIC cxx_std_20)
set_target_properties(icarus_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if (MSVC)
    target_compile_options(icarus_core PRIVATE /W4 /fp:precise)
else()
    # -ffp-contract=off keeps floating point results identical between builds (no FMA fusion).
    target_compile_options(icarus_core PRIVATE -Wall -Wextra -Wno-unused-parameter -ffp-contract=off)
endif()
