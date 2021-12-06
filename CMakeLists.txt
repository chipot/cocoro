cmake_minimum_required(VERSION 3.19)

project(cocoro)

include(CheckIPOSupported)

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
check_ipo_supported()

set(CMAKE_CXX_STANDARD 20)
add_compile_options($<$<CXX_COMPILER_ID:GNU>:-fdiagnostics-color=always>)

add_link_options(-fuse-ld=gold)

add_compile_options($<$<CXX_COMPILER_ID:GNU>:-fsanitize=address,undefined>)
add_link_options($<$<CXX_COMPILER_ID:GNU>:-fsanitize=address,undefined>)


set(EVENT__DISABLE_BENCHMARK ON)
set(EVENT__DISABLE_REGRESS ON)
set(EVENT__DISABLE_TESTS ON)
set(EVENT__DISABLE_SAMPLES ON)
add_subdirectory(libs/libevent)

add_executable(timer)
file(GLOB SRVSRC CONFIGURE_DEPENDS src/*.cpp)
target_sources(timer PRIVATE ${SRVSRC})
target_include_directories(timer PRIVATE src)
target_link_libraries(timer PRIVATE event_core_static)