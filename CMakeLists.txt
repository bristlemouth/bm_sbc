cmake_minimum_required(VERSION 3.18)
project(bm_sbc LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
set(BM_SBC_APP "" CACHE STRING "Name of a single app under apps/ to build")
option(BM_SBC_BUILD_ALL_APPS "Build every app found under apps/" OFF)

# ---------------------------------------------------------------------------
# bm_core integration (section 4 of PLAN.md)
# ---------------------------------------------------------------------------
# The submodule lives at lib/bm_core.  Its CMake helpers expose
# setup_bm_ip_stack() and setup_bm_os() after we include the entry-point
# file.  Until the submodule is populated these calls are stubbed out so the
# skeleton still configures cleanly.

set(BM_CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib/bm_core)

include(${BM_CORE_DIR}/cmake/bm_core.cmake)

# Provide include paths that the LINUX / POSIX backends need.
# bm_core expects the integrator to supply bm_config.h (lives in src/core/).
set(BM_SBC_PLATFORM_INCLUDES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core
  ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/linux
)

setup_bm_ip_stack(LINUX "${BM_SBC_PLATFORM_INCLUDES}")
setup_bm_os(POSIX "${BM_SBC_PLATFORM_INCLUDES}")

# Build bm_core in hosted (non-embedded) mode — skips ADIN2111 driver,
# guards ELF-specific attributes, etc.
set(BM_HOSTED ON CACHE BOOL "Build bm_core for hosted environment (Linux/macOS)")

add_subdirectory(${BM_CORE_DIR})

# ---------------------------------------------------------------------------
# bm_sbc_core – shared stack runtime library (section 3.1)
# ---------------------------------------------------------------------------
add_library(bm_sbc_core STATIC
  src/core/runtime.cpp
  src/core/app_runner.cpp
  src/platform/linux/platform_linux.cpp
  src/net/virtual_port_device.cpp
  src/net/gateway_device.cpp
  src/transports/uart_l2/cobs.c
  src/transports/uart_l2/crc32c.c
  src/transports/uart_l2/frame_codec.c
  src/transports/uart_l2/uart_l2_transport.cpp
)

target_include_directories(bm_sbc_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/linux
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net
    ${CMAKE_CURRENT_SOURCE_DIR}/src/transports/uart_l2
)

target_link_libraries(bm_sbc_core PUBLIC bmcore)

# ---------------------------------------------------------------------------
# App selection helpers (section 5)
# ---------------------------------------------------------------------------

# build_app(<app_name>)
#   Adds an executable target `bm_sbc_<app_name>` from apps/<app_name>/.
function(build_app APP_NAME)
  set(APP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/apps/${APP_NAME})
  if(NOT IS_DIRECTORY ${APP_DIR})
    message(FATAL_ERROR "App directory not found: ${APP_DIR}")
  endif()

  # Collect all .cpp sources in the app directory.
  file(GLOB APP_SOURCES ${APP_DIR}/*.cpp)
  if(NOT APP_SOURCES)
    message(FATAL_ERROR "No .cpp sources found in ${APP_DIR}")
  endif()

  set(APP_TARGET bm_sbc_${APP_NAME})
  add_executable(${APP_TARGET}
    src/core/main.cpp
    ${APP_SOURCES}
  )

  # bm_sbc_core contains platform stubs (platform_linux.cpp)
  # that bmcore/bcmp references.
  # GNU ld (Linux) is single-pass, so those object files are
  # silently dropped unless something already references them.
  # Force the whole archive on Linux to break the circular dependency.
  # macOS's linker is multi-pass and handles this automatically.
  target_link_libraries(${APP_TARGET} PRIVATE
    "$<$<PLATFORM_ID:Linux>:-Wl,--push-state,--whole-archive>"
    bm_sbc_core
    "$<$<PLATFORM_ID:Linux>:-Wl,--pop-state>"
  )
  target_include_directories(${APP_TARGET} PRIVATE ${APP_DIR})
endfunction()

# --- Single-app mode ---
if(BM_SBC_APP)
  build_app(${BM_SBC_APP})

  # Convenience alias: the default node executable name.
  add_executable(bm_sbc_node ALIAS bm_sbc_${BM_SBC_APP})

# --- Build-all mode ---
elseif(BM_SBC_BUILD_ALL_APPS)
  file(GLOB APP_DIRS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/apps
       ${CMAKE_CURRENT_SOURCE_DIR}/apps/*)
  foreach(APP_ENTRY ${APP_DIRS})
    if(IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/apps/${APP_ENTRY})
      build_app(${APP_ENTRY})
    endif()
  endforeach()

  # Default node target points to the first app alphabetically.
  list(SORT APP_DIRS)
  list(GET APP_DIRS 0 FIRST_APP)
  add_executable(bm_sbc_node ALIAS bm_sbc_${FIRST_APP})

# --- No app selected – build the example by default ---
else()
  if(IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/apps/example)
    build_app(example)
    add_executable(bm_sbc_node ALIAS bm_sbc_example)
  else()
    message(STATUS
      "No app selected and no apps/example/ found. "
      "Use -DBM_SBC_APP=<name> or -DBM_SBC_BUILD_ALL_APPS=ON.")
  endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()

add_executable(test_frame_codec
  tests/test_frame_codec.c
  src/transports/uart_l2/cobs.c
  src/transports/uart_l2/crc32c.c
  src/transports/uart_l2/frame_codec.c
)
target_include_directories(test_frame_codec PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src/transports/uart_l2
)
add_test(NAME frame_codec COMMAND test_frame_codec)

