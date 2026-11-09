cmake_minimum_required(VERSION 3.20)
project(native_suatom LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT DEFINED REPO_ROOT OR NOT DEFINED SPIRV_HEADERS_ROOT)
    message(FATAL_ERROR "REPO_ROOT and SPIRV_HEADERS_ROOT are required")
endif()
set(SHADER "${REPO_ROOT}/gpu/vk/eden/shader_recompiler")
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    "${SHADER}/backend/spirv/*.cpp"
    "${SHADER}/frontend/ir/*.cpp"
    "${SHADER}/frontend/maxwell/*.cpp"
    "${SHADER}/ir_opt/*.cpp"
    "${REPO_ROOT}/gpu/vk/sirit/src/*.cpp")
add_executable(native_suatom "${REPO_ROOT}/tests/native_suatom.cpp" ${SOURCES})
target_include_directories(native_suatom PRIVATE
    "${REPO_ROOT}/gpu/vk/compat" "${REPO_ROOT}/gpu/compat"
    "${REPO_ROOT}/gpu/vk/eden" "${REPO_ROOT}/gpu/third_party/suyu"
    "${REPO_ROOT}/gpu/vk/sirit/include" "${REPO_ROOT}/gpu/vk/sirit/src"
    "${SPIRV_HEADERS_ROOT}/include")
target_compile_definitions(native_suatom PRIVATE NDEBUG MKWVK_INCLUDES=2 VK_NO_PROTOTYPES)
if(MSVC)
    target_compile_options(native_suatom PRIVATE /W0 /O1 /EHsc)
else()
    target_compile_options(native_suatom PRIVATE -w -O1)
endif()
