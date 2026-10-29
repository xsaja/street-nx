# dk_shaders: the deko3d feasibility check on the PC. Takes the SPIR-V the Vulkan renderer caches (shadercache/*.spv),
# turns each into GLSL with SPIRV-Cross the way the deko3d backend will, and compiles it with uam to DKSH.
# Build with MSYS2 MINGW64 (gcc): cmake -S tools/dk_shaders -B build-dk -G Ninja && ninja -C build-dk
cmake_minimum_required(VERSION 3.20)
project(dk_shaders C CXX)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(THIRD ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party)
add_subdirectory(${THIRD}/uam ${CMAKE_CURRENT_BINARY_DIR}/uam)

set(SPVC ${THIRD}/SPIRV-Cross)
add_library(spirv_cross_glsl STATIC ${SPVC}/spirv_cross.cpp ${SPVC}/spirv_parser.cpp ${SPVC}/spirv_cross_parsed_ir.cpp
                                    ${SPVC}/spirv_cfg.cpp ${SPVC}/spirv_glsl.cpp)
target_include_directories(spirv_cross_glsl PUBLIC ${SPVC})
set_target_properties(spirv_cross_glsl PROPERTIES CXX_STANDARD 17)

add_executable(dk_shaders dk_shaders.cpp ../../switch/dk_glsl.cpp)
target_include_directories(dk_shaders PRIVATE ../../switch)
set_target_properties(dk_shaders PROPERTIES CXX_STANDARD 17)
target_link_libraries(dk_shaders PRIVATE spirv_cross_glsl uamlib)
target_link_options(dk_shaders PRIVATE -static -Wl,--stack,8388608)
