cmake_minimum_required(VERSION 3.22)
project(SchwungAU VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)

# Schwung host source: plugin_api_v1.h & friends, the chain host and the
# built-in modules. Use -DSCHWUNG_SRC=<checkout> (tools/build_mac.sh passes the
# one it fetched), a checkout in third_party/schwung, or fetch the commit
# pinned in modules.lock.json.
set(SCHWUNG_PINNED_COMMIT "75f04236f6110b6a34e77417b167e3fc962ff803")
if(NOT SCHWUNG_SRC)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/schwung/src/host/plugin_api_v1.h")
        set(SCHWUNG_SRC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/schwung")
    else()
        FetchContent_Declare(schwung_host
            GIT_REPOSITORY https://github.com/charlesvestal/schwung.git
            GIT_TAG ${SCHWUNG_PINNED_COMMIT}
            GIT_SHALLOW FALSE)
        FetchContent_Populate(schwung_host)
        set(SCHWUNG_SRC "${schwung_host_SOURCE_DIR}")
    endif()
endif()
set(SCHWUNG_SRC "${SCHWUNG_SRC}" CACHE PATH "Schwung host source tree" FORCE)
message(STATUS "Schwung source: ${SCHWUNG_SRC}")

option(SCHWUNG_BUILD_TESTS  "Build headless test tools" ON)
option(SCHWUNG_BUILD_PLUGIN "Build the JUCE plugin (AU/AUv3/VST3/Standalone)" ON)

# ---- core: engine + catalog (no JUCE) --------------------------------------
add_library(schwung_core STATIC
    core/src/Engine.cpp
    core/src/Catalog.cpp)
target_include_directories(schwung_core
    PUBLIC  core/include
    PRIVATE core/src core/third_party "${SCHWUNG_SRC}/src")
target_include_directories(schwung_core PUBLIC core/third_party)
find_package(Threads REQUIRED)
target_link_libraries(schwung_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # plugin_api_v1.h uses C11 _Static_assert; fine in C++ as an extension.
    target_compile_options(schwung_core PRIVATE -Wno-c11-extensions -Wno-unknown-warning-option)
endif()

if(SCHWUNG_BUILD_TESTS)
    add_executable(sw_render tests/sw_render.cpp)
    target_link_libraries(sw_render PRIVATE schwung_core)
    # Constant-latency and MIDI-timing regression test (needs built modules).
    add_executable(sw_latency tests/sw_latency.cpp)
    target_link_libraries(sw_latency PRIVATE schwung_core)
endif()

if(SCHWUNG_BUILD_PLUGIN)
    add_subdirectory(plugin)
endif()
