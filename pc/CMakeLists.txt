# STREET-vk.exe: MVP's Windows build (sdk/runtime/app: window, settings, SDL3 input, XAudio2) with the Switch's Vulkan
# backend (switch/vulkan.cpp) in place of gpu/d3d12.cpp, to check that backend on the PC against D3D12 (Phase 2).
# Also builds xbe_recomp and recompiles game/assets/default.xbe into game/generated/game, which the Switch build takes as it is.
#
# From an x64 Visual Studio prompt with clang, clang++, lld, cmake and ninja (tools/build_pc.cmd does this):
# cmake -S pc -B build-pc -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C build-pc STREET-vk

cmake_minimum_required(VERSION 3.25)
project(street_pc LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "Configure with clang/clang++ (-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++)")
endif()
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=x86-64-v2")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=x86-64-v2")

get_filename_component(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)
set(THIRD ${ROOT}/third_party)
set(HOST ${ROOT}/switch)

# MVP's vendored dependencies, offline
set(FETCHCONTENT_SOURCE_DIR_ZYDIS ${THIRD}/zydis)
set(FETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS ${THIRD}/tomlplusplus)
set(FETCHCONTENT_SOURCE_DIR_IMGUI ${THIRD}/imgui)
set(FETCHCONTENT_SOURCE_DIR_SDL3 ${THIRD}/sdl3)
set(FETCHCONTENT_FULLY_DISCONNECTED ON)
include(${ROOT}/sdk/cmake/XbeRecomp.cmake)

# the first build has no config yet: only the tools, so xbe_symbols can write it (tools/build_pc.cmd)
if(NOT EXISTS ${ROOT}/game/config/game.toml)
    message(STATUS "No game/config/game.toml yet: only xbe_recomp and xbe_symbols")
    return()
endif()

# glslang with its HLSL front end, as the Switch build has it
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL ON CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(${THIRD}/glslang ${CMAKE_CURRENT_BINARY_DIR}/glslang EXCLUDE_FROM_ALL)

# no Vulkan SDK on this PC: an import library for the driver's loader (System32\vulkan-1.dll) from its exports
set(VK_LIB ${CMAKE_CURRENT_BINARY_DIR}/vulkan-1.lib)
if(NOT EXISTS ${VK_LIB})
    find_program(DUMPBIN dumpbin REQUIRED)
    find_program(LIB_EXE lib REQUIRED)
    execute_process(COMMAND ${DUMPBIN} /exports C:/Windows/System32/vulkan-1.dll OUTPUT_VARIABLE exports)
    string(REGEX MATCHALL "\n +[0-9]+ +[0-9A-F]+ [0-9A-F]+ (vk[A-Za-z0-9_]+)" lines "${exports}")
    set(def "LIBRARY vulkan-1\nEXPORTS\n")
    foreach(line ${lines})
        string(REGEX REPLACE ".* (vk[A-Za-z0-9_]+)$" "\\1" name "${line}")
        string(APPEND def "    ${name}\n")
    endforeach()
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/vulkan-1.def "${def}")
    execute_process(COMMAND ${LIB_EXE} /nologo /def:${CMAKE_CURRENT_BINARY_DIR}/vulkan-1.def /machine:x64 /out:${VK_LIB}
                    RESULT_VARIABLE rc)
    if(rc)
        message(FATAL_ERROR "vulkan-1.lib could not be made")
    endif()
endif()

# the recompiled code, shared with the Switch build
set(GEN ${ROOT}/game/generated/game)
file(STRINGS ${ROOT}/game/config/game.toml GAME_PARTITIONS REGEX "^partitions *=")
string(REGEX MATCH "[0-9]+" GAME_PARTITIONS "${GAME_PARTITIONS}")
math(EXPR last "${GAME_PARTITIONS} - 1")
set(GEN_SOURCES ${GEN}/x86_func_mapping.cpp)
foreach(i RANGE ${last})
    list(APPEND GEN_SOURCES ${GEN}/x86_recomp.${i}.cpp)
endforeach()
file(GLOB configs CONFIGURE_DEPENDS ${ROOT}/game/config/*.toml ${XBE_SDK_DIR}/xdk/*/*.toml)
add_custom_command(
    OUTPUT ${GEN}/codegen.stamp
    BYPRODUCTS ${GEN_SOURCES} ${GEN}/x86_recomp_shared.h ${GEN}/image_symbols.h
    COMMAND xbe_recomp config/game.toml --xdk ${XBE_SDK_DIR}/xdk
    COMMAND ${CMAKE_COMMAND} -E touch ${GEN}/codegen.stamp
    DEPENDS xbe_recomp ${configs} ${ROOT}/game/assets/default.xbe
    WORKING_DIRECTORY ${ROOT}/game
    COMMENT "Recompiling default.xbe"
    VERBATIM)
add_custom_target(recompile DEPENDS ${GEN}/codegen.stamp)
set_source_files_properties(${GEN_SOURCES} PROPERTIES GENERATED TRUE
    COMPILE_OPTIONS "-O2;-fno-strict-aliasing;-ffp-contract=off;-Wno-unused-label;-Wno-parentheses-equality;-Wno-unused-variable")

# the recompiled code once, for both executables
add_library(street_gen OBJECT ${GEN_SOURCES})
add_dependencies(street_gen recompile)
target_include_directories(street_gen PRIVATE ${ROOT}/game/src ${XBE_SDK_DIR}/runtime ${GEN})
target_compile_options(street_gen PRIVATE -fno-strict-aliasing)
target_compile_definitions(street_gen PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)

file(GLOB GAME_SOURCES CONFIGURE_DEPENDS ${ROOT}/game/src/*/*.cpp)
set(IMGUI_COMMON ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp)
set(VK_SOURCES ${HOST}/vulkan.cpp ${HOST}/vk_shader.cpp ${CMAKE_CURRENT_SOURCE_DIR}/host_win.cpp)
set_source_files_properties(${HOST}/vk_shader.cpp PROPERTIES COMPILE_DEFINITIONS ENABLE_HLSL)
# the SDK's sources take Vulkan from the Khronos headers, as the Switch build does before NVK
set_source_files_properties(${VK_SOURCES} ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${THIRD}/Vulkan-Headers/include;${HOST};${THIRD}/glslang")

# every log level in this release build, to compare a run line by line with the Switch's
option(STREET_VERBOSE_LOG "log info and debug lines" OFF)

# MVP's Windows host and runtime around one renderer: the extra sources and libraries are the backend's
function(add_street target)
    cmake_parse_arguments(A "" "" "SOURCES;LIBS" ${ARGN})
    add_executable(${target} WIN32 ${A_SOURCES} ${GAME_SOURCES} ${IMGUI_COMMON} $<TARGET_OBJECTS:street_gen>)
    target_include_directories(${target} PRIVATE ${ROOT}/game/src ${XBE_SDK_DIR}/runtime ${GEN})
    target_include_directories(${target} SYSTEM PRIVATE ${imgui_SOURCE_DIR})
    target_compile_options(${target} PRIVATE -fno-strict-aliasing)
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
    if(STREET_VERBOSE_LOG)
        target_compile_definitions(${target} PRIVATE XBE_VERBOSE_LOG)
    endif()
    target_link_libraries(${target} PRIVATE tomlplusplus::tomlplusplus $<LINK_ONLY:SDL3::SDL3-static> ${A_LIBS}
        xaudio2 ole32 windowscodecs dbghelp winmm dwmapi ws2_32 dxguid)
    # lld keeps the recompiled weak functions overridable by the host ones
    target_link_options(${target} PRIVATE -fuse-ld=lld -g)
endfunction()

# the Switch's Vulkan backend in place of D3D12
set(RUNTIME_VK ${XBE_RUNTIME_SOURCES})
list(FILTER RUNTIME_VK EXCLUDE REGEX "/gpu/d3d12\.cpp$")
add_street(STREET-vk SOURCES ${RUNTIME_VK} ${VK_SOURCES} ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
          LIBS glslang glslang-default-resource-limits ${VK_LIB})
# MVP's own D3D12 renderer from the same sources, the reference the captures are compared with
add_street(STREET-d3d12 SOURCES ${XBE_RUNTIME_SOURCES} ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp
          LIBS d3d12 dxgi d3dcompiler)
