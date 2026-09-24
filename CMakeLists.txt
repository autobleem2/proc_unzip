# proc_unzip: an AutoBleem scanner processor - unpacks zipped PS1 games and ROMs on the stick. A processor is a
# plain console program: no AutoBleem SDK, no SDL, nothing but the C++ standard library and miniz, so it builds
# with any toolchain and is the example to copy (docs/scanner-processors-plan.md in the launcher).
cmake_minimum_required(VERSION 3.12)
project(proc_unzip C CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# the console's Cortex-A35 (ARMv8-A, AArch32, NEON), as the launcher builds for it
if (AB_TARGET STREQUAL "psc")
    if (CMAKE_CXX_COMPILER_VERSION VERSION_GREATER 8.0)
        set(CMAKE_C_FLAGS "-mfloat-abi=hard -march=armv8-a+simd -Os -s")
        set(CMAKE_CXX_FLAGS "-mfloat-abi=hard -march=armv8-a+simd -Os -s")
    else()
        set(CMAKE_C_FLAGS "-mfloat-abi=hard -march=armv8-a -mfpu=neon-vfpv4 -Os -s")
        set(CMAKE_CXX_FLAGS "-mfloat-abi=hard -march=armv8-a -mfpu=neon-vfpv4 -Os -s")
    endif()
endif()

add_library(miniz STATIC third_party/miniz/miniz.c)
target_include_directories(miniz PUBLIC third_party/miniz)
set_source_files_properties(third_party/miniz/miniz.c PROPERTIES COMPILE_FLAGS "-w")
target_compile_definitions(miniz PUBLIC MINIZ_NO_TIME)

add_executable(unzip src/main.cpp)
target_link_libraries(unzip miniz)
if (MINGW OR WIN32)
    # one file to copy onto the stick: no libstdc++/libgcc DLLs next to it
    target_link_options(unzip PRIVATE -static)
endif()
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(unzip PRIVATE -Wall -Wextra)
endif()

# the self-test: real zips, the real program, what the protocol and the rules promise
option(PROC_BUILD_TESTS "build and register the self-test" ON)
if (PROC_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
    enable_testing()
    add_executable(unzip_selftest tests/selftest.cpp)
    target_link_libraries(unzip_selftest miniz)
    target_compile_definitions(unzip_selftest PRIVATE UNZIP_EXE="$<TARGET_FILE:unzip>")
    add_dependencies(unzip_selftest unzip)
    add_test(NAME unzip_selftest COMMAND unzip_selftest WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif()
