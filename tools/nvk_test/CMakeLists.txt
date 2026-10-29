# nvk_test.nro: dk_test's GPU measure on NVK, for comparison (main.cpp). Built with devkitPro's toolchain by build.sh,
# linked with NVK as the game is (switch/CMakeLists.txt).
cmake_minimum_required(VERSION 3.20)
project(street_nvk_test C CXX)

get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(THIRD "${ROOT}/third_party")
set(NVK_SDK "${ROOT}/nvk_sdk")

set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)
add_subdirectory(${THIRD}/glslang ${CMAKE_CURRENT_BINARY_DIR}/glslang EXCLUDE_FROM_ALL)

add_executable(nvk_test main.cpp ${ROOT}/switch/switch_elf_stubs.c)
target_include_directories(nvk_test PRIVATE ${NVK_SDK}/include ${THIRD}/glslang)
set_target_properties(nvk_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_options(nvk_test PRIVATE -O2 -Wall)
file(GLOB NVK_ARCHIVES ${NVK_SDK}/lib/lib*.a)
list(FILTER NVK_ARCHIVES EXCLUDE REGEX "/lib(EGL|GL|GLESv1_CM|GLESv2|glapi)\\.a$")
list(REMOVE_ITEM NVK_ARCHIVES ${NVK_SDK}/lib/libvulkan.a)
target_link_libraries(nvk_test PRIVATE glslang glslang-default-resource-limits
    -Wl,--allow-multiple-definition
    -Wl,--start-group ${NVK_SDK}/lib/libvulkan.a ${NVK_ARCHIVES} zstd expat z nx stdc++ m pthread -Wl,--end-group)

nx_generate_nacp(nvk_test.nacp NAME "MVP NVK test" AUTHOR "mvp-nx" VERSION "0.1.0")
nx_create_nro(nvk_test NACP nvk_test.nacp)
