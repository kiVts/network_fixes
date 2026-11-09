cmake_minimum_required(VERSION 3.24)
project(network_fixes LANGUAGES C CXX)

# Needs the SWML SDK repo: cmake -B build -A x64 -DSWML_SDK_DIR=path/to/swml_sdk -DSWML_GAME_DATA_DIR=path/to/generated/headers
set(SWML_SDK_DIR "" CACHE PATH "SWML SDK folder (the one with SwmlMod.cmake)")
if(NOT EXISTS "${SWML_SDK_DIR}/SwmlMod.cmake")
  message(FATAL_ERROR "Set SWML_SDK_DIR to the SWML SDK folder")
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_subdirectory("${SWML_SDK_DIR}" swml_sdk)

include(FetchContent)
set(LIBDEFLATE_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_GZIP OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libdeflate GIT_REPOSITORY https://github.com/ebiggers/libdeflate.git GIT_TAG v1.24)
FetchContent_MakeAvailable(libdeflate)

swml_add_mod(network_fixes SOURCES
  src/setup.cpp
  src/steamnet.cpp
  src/netpath.cpp
  src/playout.cpp
  src/redundancy.cpp
  src/forecast_measure.cpp
  src/pacing.cpp
  src/audit.cpp
  src/remote_push.cpp
  src/host_watch.cpp
  src/version_check.cpp
  src/map_catchup.cpp
  src/mp_watch.cpp
  src/catchup_banner.cpp
  src/correction_rate.cpp
  src/own_echo.cpp
  src/local_first.cpp
  src/tick_bytes.cpp
  src/object_push.cpp
  src/editor_background.cpp
  src/touch_census.cpp
)
target_include_directories(network_fixes PRIVATE src)
target_compile_definitions(network_fixes PRIVATE SWML_NET_FIXES=1 SWML_NET_DIAG=0)
target_link_libraries(network_fixes PRIVATE libdeflate_static)
