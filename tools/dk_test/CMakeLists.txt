# dk_test.nro: deko3d stage D1 on the console (main.cpp). Built with devkitPro's toolchain by build.sh.
# Modeled on NFSMW-NX's tools/switch/dk_prueba (GPL-3.0).
cmake_minimum_required(VERSION 3.20)
project(street_dk_test C CXX)

get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(THIRD "${ROOT}/third_party")
set(HOST "${ROOT}/switch")

add_subdirectory("${THIRD}/uam" uam EXCLUDE_FROM_ALL)
set(SPVC "${THIRD}/SPIRV-Cross")
add_library(spirv_cross_glsl STATIC ${SPVC}/spirv_cross.cpp ${SPVC}/spirv_parser.cpp ${SPVC}/spirv_cross_parsed_ir.cpp
                                    ${SPVC}/spirv_cfg.cpp ${SPVC}/spirv_glsl.cpp)
target_include_directories(spirv_cross_glsl PUBLIC ${SPVC})
set_target_properties(spirv_cross_glsl PROPERTIES CXX_STANDARD 17)
target_compile_options(spirv_cross_glsl PRIVATE -O2 -w)

add_executable(dk_test main.cpp "${HOST}/dk_core.cpp" "${HOST}/dk_glsl.cpp")
target_include_directories(dk_test PRIVATE "${HOST}")
set_target_properties(dk_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_options(dk_test PRIVATE -O2 -Wall)

# DK_DEBUG (on by default for this test): deko3d's debug library checks every call and reports errors readably
option(DK_DEBUG "link deko3d's debug library (libdeko3dd)" ON)
target_link_libraries(dk_test PRIVATE uamlib spirv_cross_glsl $<IF:$<BOOL:${DK_DEBUG}>,deko3dd,deko3d>)

nx_generate_nacp(dk_test.nacp NAME "MVP deko3d test" AUTHOR "mvp-nx" VERSION "0.1.0")
nx_create_nro(dk_test NACP dk_test.nacp)
